// lb_wide2d.inc - widen a pure-2D scene across the adaptive columns.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md. Textually included INSIDE gpu2d.cpp's anonymous
// namespace, because everything it needs -- Unit, g_unit, decode_bg_line,
// render_obj_line, compose, to_rgb32 -- is private to that translation unit.
//
// WHY THIS EXISTS
//
// nds_gpu2d_adaptive_framebuffer has a real widening compositor, but it returns
// early for any engine other than engine A, centring the native 256-wide image
// in the wider buffer and leaving the extra columns black. Its supported-scene
// test also requires `mode == 1 && bg0_3d`: it is a 3D-scene-plus-2D-HUD
// widener, written for a top screen with a 3D background.
//
// LEGO Battles' game field is a 2D tilemap on the BOTTOM screen, which is
// engine B, and engine B has no 3D at all. So the one screen the port needs
// widened (docs/84, docs/107) took the letterbox branch every frame.
//
// This is the 2D counterpart: same compositing rules as compose_line6, just
// evaluated over `width` columns instead of 256.
//
// WHAT IT CAN AND CANNOT WIDEN
//
// Text backgrounds can extend, because decode_text_line already accepts a
// left_extra and decodes 256 + 2*left_extra columns straight out of the
// tilemap. Affine and extended backgrounds cannot, so a scene containing one
// falls back to the letterbox rather than inventing pixels.
//
// OBJECTS DO NOT EXTEND, and this is a property of the guest, not a shortcut
// here. The DS submits at most 128 sprites and LEGO Battles fills OAM for a
// 256-wide viewport; sprites outside it were never written, so there is nothing
// to draw in the new columns. Widening therefore shows more TERRAIN, with units
// appearing as they cross into the hardware viewport. Making units appear in
// the widened columns would mean changing what the guest culls -- a guest-side
// change, safe for lockstep (docs/85) but not a host decision, and not this
// file's job.

// MAY THE ENGINE-A WIDENER DECODE PAST THE HARDWARE VIEWPORT IN THIS TITLE?
//
// No, and this was established the hard way. Engine A's field layers BG1 and
// BG2 are 512x256, so a 512-column tilemap looked like proof that real map data
// exists out there. It is not. The game maintains tile entries only for the
// 256-column window it actually draws, plus a small scroll margin; the rest of
// the 512-column map holds whatever was last written to it. Decoding it put a
// repeating block of stale tiles either side of the field -- visible corruption,
// not more of the map.
//
// "The tilemap is wide enough" and "the tilemap is filled" are different claims,
// and only the second one is about pixels. Until the GUEST fills tiles for a
// wider span -- the same work as not culling units outside the vanilla view --
// there is nothing truthful to show out there, and lb_edge_fill is used instead.
//
// Flip this to true once the guest supplies the tiles; nothing else needs to
// change.
const bool lb_engineA_widening_allowed = false;

// True when the current refusal was the HUD-overlay case, so the fill below can
// anchor instead of stretch. Set by lb_wide2d_supported, which runs once per
// frame per engine immediately before the fill.
bool lb_scene_is_hud = false;
// docs/389: which engine the fill that follows is for (lb_wide2d_supported
// again), so each keeps its own cache in lb_radial_fill.
int lb_fill_engine = 0;

// docs/389: see lb_radial_fill, defined after lb_parallel_rows.
bool lb_radial_fill(uint32_t* out, int w, int h, int rx, int ry, int rw,
                    int rh);


// Fill the widened columns for a scene that CANNOT be extended truthfully.
//
// The menus here run in BG mode 3 -- an affine/extended background -- and an
// affine layer has no honest continuation outside the viewport: its transform
// is defined against the hardware screen, so decoding further just walks off
// the map. The alternative is what the shipped code does, which is to leave
// black bars either side of a menu on a wide window.
//
// Each ROW's edge pixel is carried outward instead. For a flat background that
// is exactly a tile of the single colour; for the vertical gradients these
// menus actually use it continues the gradient; for a patterned one it smears
// rather than inventing structure that is not there. It never fabricates
// detail, which a horizontal stretch of the whole image would, and it never
// distorts the menu's own layout.
// Carry the topmost and bottommost drawn rows outward, so a taller output has
// no black band above or below the picture.
//
// The vertical counterpart of lb_edge_fill's horizontal carry, and it exists
// for the same reason: the guest authored 192 rows and there is nothing
// truthful outside them, so the honest continuation of the picture is its own
// edge row. Menus here are vertical gradients and flat panels, both of which
// this continues exactly. It is a no-op while extra_y is 0.
void lb_fill_rows_outward(uint32_t* out, int output_width, int output_height,
                          int extra_y) {
    if (extra_y <= 0) return;
    // docs/389: a menu widened truthfully continues its own gradient above
    // and below (lb_radial_fill); the HUD over a battle keeps this carry.
    if (!lb_scene_is_hud &&
        lb_radial_fill(out, output_width, output_height, 0, extra_y,
                       output_width, 192))
        return;
    const uint32_t* const top = out + static_cast<size_t>(extra_y) * output_width;
    for (int y = 0; y < extra_y; ++y)
        std::copy_n(top, output_width,
                    out + static_cast<size_t>(y) * output_width);
    const int last = extra_y + 192 - 1;
    if (last < 0 || last >= output_height) return;
    const uint32_t* const bot = out + static_cast<size_t>(last) * output_width;
    for (int y = last + 1; y < output_height; ++y)
        std::copy_n(bot, output_width,
                    out + static_cast<size_t>(y) * output_width);
}

void lb_edge_fill(const uint32_t* native, uint32_t* out, int output_width,
                  int output_height, int extra, int extra_y) {
    // docs/389: the native picture at 1:1 in the centre, and around it its
    // own gradient continued (lb_radial_fill), not each edge carried out.
    if (extra >= 0 && extra_y >= 0 && extra + 256 <= output_width &&
        extra_y + 192 <= output_height) {
        for (int y = 0; y < 192; ++y)
            std::copy_n(native + static_cast<size_t>(y) * 256, 256,
                        out + static_cast<size_t>(y + extra_y) * output_width +
                            extra);
        if (lb_radial_fill(out, output_width, output_height, extra, extra_y,
                           256, 192))
            return;
    }
    const int right = extra + 256;
    for (int y = 0; y < 192; ++y) {
        const uint32_t* const src = native + static_cast<size_t>(y) * 256;
        uint32_t* const dst =
            out + static_cast<size_t>(y + extra_y) * output_width;
        std::copy_n(src, 256, dst + extra);
        std::fill_n(dst, extra, src[0]);
        if (output_width > right)
            std::fill_n(dst + right, output_width - right, src[255]);
    }
    lb_fill_rows_outward(out, output_width, output_height, extra_y);
}

// Can this engine's current scene be widened without inventing anything?
bool lb_wide2d_supported(int engine, const Unit& u, const uint8_t* palette,
                         const uint8_t* oam,
                         const NdsVramRendererView* vram) {
    // Say once, in the log, why a scene was refused. "The widener is compiled
    // in" and "the widener ran" are different claims, and only the second one
    // puts pixels on the screen.
    const char* why = nullptr;
    lb_scene_is_hud = false;
    lb_fill_engine = engine & 1;   // docs/389
    if (!palette || !oam || !vram) why = "no palette/oam/vram";
    else if (u.dispcnt & 0x80u) why = "forced blank";
    // Engine B only has display modes 0 and 1; 1 is the graphics mode.
    else if (((u.dispcnt >> 16) & (engine ? 1u : 3u)) != 1u) why = "display mode";
    // Window regions are defined against the hardware viewport. Extending a
    // windowed composite would apply the wrong region to the new columns.
    else if (u.dispcnt & 0xE000u) why = "windows enabled";
    // The 3D path belongs to the engine-A compositor above; do not shadow it.
    else if (engine == 0 && (u.dispcnt & 0x8u) && (u.dispcnt & 0x100u))
        why = "engine A 3D scene";
    else if (g_present_capture_active) why = "present capture active";
    // A 2D scene that is a HUD OVERLAY on a 3D scene must not be extended.
    //
    // Extending a 256-wide text BG to 448 is exactly TILING it: the map is 32
    // columns, so the extra columns repeat what is already on screen. For a
    // menu backdrop that is correct and invisible -- the starfield simply
    // continues. For the in-game HUD it duplicates content: a second gold
    // counter, a second population readout, a second minimap blip. That is the
    // "corrupts beyond boundary" defect, and upstream's own comment
    // on nds_gpu2d_set_adaptive_bg_extend says the same thing -- extend only
    // "while the 2D layers ARE the picture", not while they overlay 3D.
    //
    // The test is whether the OTHER engine is running a 3D scene, which is what
    // distinguishes gameplay from a menu. In gameplay engine A draws the field
    // with BG0 in 3D mode (docs/114); in the menus nothing does.
    else if (engine != 0 &&
             (g_unit[0].dispcnt & 0x8u) && (g_unit[0].dispcnt & 0x100u)) {
        why = "HUD over a 3D scene";
        lb_scene_is_hud = true;
    }
    else {
        for (int bg = 0; bg < 4; ++bg) {
            if (!(u.dispcnt & (0x100u << bg))) continue;
            const BgKind kind = bg_kind(u, bg);
            if (kind == BgKind::None) continue;
            if (kind != BgKind::Text) { why = "a non-text BG"; break; }
            // Mosaic-X is only defined over the hardware viewport, and
            // decode_text_line says so by reporting native width for it.
            if ((u.bgcnt[bg] & 0x40u) && u.bg_mosaic_x) {
                why = "mosaic-x";
                break;
            }
        }
    }
    // Report on CHANGE, not once. A one-shot latches on whatever the very
    // first frame looked like -- at boot that is DISPCNT 0, "display mode",
    // which says nothing about the scene anyone cares about.
    static const char* last[2] = {reinterpret_cast<const char*>(1),
                                  reinterpret_cast<const char*>(1)};
    const int slot = engine & 1;
    const char* now = why ? why : "WIDENING";
    if (last[slot] != now) {
        last[slot] = now;
        std::fprintf(stderr, "[lb_wide2d] engine %d: %s (dispcnt %08X)\n",
                     engine, now, (unsigned)u.dispcnt);
    }
    return why == nullptr;
}

// One widened scanline. Mirrors compose_line6's ordering and blending exactly;
// the only differences are the column count and the left_extra offset.
void lb_wide2d_line(int engine, int y, Unit& u, const uint8_t* palette,
                    const uint8_t* oam, const NdsVramRendererView& vram,
                    uint32_t* out, int width, int left_extra) {
    // Objects are rendered at native width and placed at their own hardware
    // columns. See the header comment: OAM has nothing outside the viewport.
    std::array<Pixel, 256> obj{};
    render_obj_line(engine, y, obj.data(), 256, oam, palette, vram);

    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{rgb6(backdrop15), 0x20u, 0, 4, 5, true};

    static std::array<BgLine, 4> bg_lines;
    size_t bg_count = 0;
    int active_bgs[4];
    for (int bg = 0; bg < 4; ++bg) {
        if (!(u.dispcnt & (0x100u << bg))) continue;
        if (bg_kind(u, bg) != BgKind::None) active_bgs[bg_count++] = bg;
    }
    for (size_t i = 0; i < bg_count; ++i) {
        decode_bg_line(engine, active_bgs[i], y, palette, vram, bg_lines[i],
                       left_extra);
    }

    const uint32_t mbmode = u.master_bright >> 14;
    const uint32_t mb = std::min<uint32_t>(16, u.master_bright & 0x1Fu);

    auto ahead = [](const Pixel& a, const Pixel& b) {
        return a.priority < b.priority ||
               (a.priority == b.priority && a.order < b.order);
    };

    for (int x = 0; x < width; ++x) {
        const int hw = x - left_extra;   // the hardware column, or outside it
        Pixel top = backdrop, below = backdrop;
        auto push = [&](const Pixel& p) {
            if (ahead(p, top)) { below = top; top = p; }
            else if (ahead(p, below)) { below = p; }
        };
        for (size_t i = 0; i < bg_count; ++i) {
            const BgLine& l = bg_lines[i];
            int idx = x;
            if (l.decoded_width <= 256) {
                // A layer that declined to extend is simply absent out here,
                // rather than being repeated or guessed at.
                if (hw < 0 || hw >= 256) continue;
                idx = hw;
            }
            const uint16_t c = l.color[idx];
            if (!(c & 0x8000u)) continue;
            push(Pixel{rgb6(static_cast<uint16_t>(c & 0x7FFFu)),
                       l.target, 0, l.prio, l.order, true});
        }
        if (hw >= 0 && hw < 256 && obj[hw].valid) push(obj[hw]);
        uint32_t c = compose(u, top, below);
        if (mbmode == 1) c = brighten(c, mb, 0u);
        else if (mbmode == 2) c = darken(c, mb, 15u);
        out[x] = to_rgb32(c);
    }
}

// ANCHOR a HUD across the widened frame.
//
// The requirement is that elements stuck to a screen edge or corner snap to
// the new leftmost or rightmost edge or corner as the window changes.
// Stretching does not do that -- it scales the icons and leaves them at the
// same fractional position. Anchoring moves them.
//
// The native 256 columns are cut into three: a left band anchored to the new
// left edge, a right band anchored to the new right edge, and a centre band
// left centred. The two gaps this opens are filled by REPLICATING the column at
// the inner edge of each band. For a HUD built from horizontal bars and panels
// -- which this one is -- replicating a column continues the bar exactly, with
// no scaling and no seam.
//
// center_width is how much of the middle stays centred. It must be small enough
// that the left and right bands still contain the edge-anchored elements, and
// large enough to hold whatever is genuinely centred: here the population
// readout sits near the middle, so the default keeps 96 columns.
void lb_hud_anchor(const uint32_t* native, uint32_t* out, int output_width,
                   int output_height, int center_width, int extra_y) {
    if (center_width < 8) center_width = 8;
    if (center_width > 256) center_width = 256;
    const int band = (256 - center_width) / 2;      // left and right band width
    const int cx = (output_width - center_width) / 2;   // centre band position
    for (int y = 0; y < 192; ++y) {
        const uint32_t* const src = native + static_cast<size_t>(y) * 256;
        uint32_t* const dst =
            out + static_cast<size_t>(y + extra_y) * output_width;
        // left gap: continue the left band's inner column outward
        std::fill_n(dst + band, cx - band > 0 ? cx - band : 0,
                    src[band > 0 ? band - 1 : 0]);
        // right gap: continue the right band's inner column
        const int rstart = cx + center_width;
        const int rband = output_width - band;
        if (rband - rstart > 0)
            std::fill_n(dst + rstart, rband - rstart, src[256 - band]);
        // the three bands themselves, unscaled
        std::copy_n(src, band, dst);                              // left edge
        std::copy_n(src + band, center_width, dst + cx);           // centre
        std::copy_n(src + 256 - band, band, dst + rband);          // right edge
    }
    lb_fill_rows_outward(out, output_width, output_height, extra_y);
}

// Fill the widened columns of a scene that CANNOT be widened truthfully.
//
// The menus run in BG mode 3 -- an affine/extended background -- whose transform
// is defined against the hardware screen, so decoding further just walks off the
// map. lb_wide2d_supported refuses them, and the shipped behaviour is then black
// bars either side of every menu on a wide window.
//
// The requirement for this case: stretch the background, or tile it if it is a
// single colour. Both halves are implemented, chosen per FRAME rather than
// per row, because mixing the two down a single image tears it vertically --
// some rows continued flat while their neighbours scale is worse than either
// treatment applied consistently.
//
//   * every row a single colour  -> fill outward with that colour. This is the
//     flat-background case and it invents nothing: the continuation of a solid
//     field is that solid field.
//
//   * otherwise                  -> stretch the whole image horizontally. This
//     is what the requirement asks for and it keeps the menu's own proportions
//     internally consistent, unlike smearing the edge pixels outward, which was
//     tried before and rejected as looking like a lazy stretch of the sides.
//
// Note this is deliberately NOT applied to the game field. docs/117 explains why
// the field cannot be widened at all yet, and stretching gameplay is exactly
// what was rejected. This path only runs for scenes the widener refuses.
void lb_menu_fill(const uint32_t* native, uint32_t* out, int output_width,
                  int output_height, int extra, int extra_y) {
    {   // docs/389: LB_FILLTRACE=1, one line whenever the call changes
        static const bool on = std::getenv("LB_FILLTRACE") != nullptr;
        static int last = -1;
        const int key = (lb_fill_engine << 28) ^ (extra << 16) ^ (extra_y << 4) ^
                        (lb_scene_is_hud ? 1 : 0) ^ output_width * 7;
        if (on && key != last) {
            last = key;
            std::fprintf(stderr, "[lb_filltrace] engine %d %dx%d at %d,%d "
                         "hud=%d dispcnt %08X\n", lb_fill_engine, output_width,
                         output_height, extra, extra_y, lb_scene_is_hud ? 1 : 0,
                         (unsigned)g_unit[lb_fill_engine & 1].dispcnt);
        }
    }
    if (lb_scene_is_hud) {
        lb_hud_anchor(native, out, output_width, output_height, 96, extra_y);
        return;
    }
    // NOT A HORIZONTAL STRETCH.
    //
    // This used to resample the whole native image across the output width:
    //
    //     sx = (x * 255) / (output_width - 1)
    //
    // which is a 448/256 = 1.75x horizontal scale of everything on screen. The
    // menus came out visibly wide -- every glyph, button and portrait stretched
    // -- and it is exactly what must not happen: the vanilla UI
    // is to keep its own aspect and stay centred, with only the BACKDROP made
    // to cover the extra width.
    //
    // It was measurable, not just a matter of taste. On the Single Player menu
    // the Back button occupies native columns 9..33; under the stretch it
    // landed at output columns 16..58, which is where a capture found it. At
    // 1:1 centred it belongs at 105..129.
    //
    // lb_edge_fill draws the native image at 1:1 in the centre and carries each
    // ROW's own edge pixel outward. For the vertical gradients these menus
    // actually use that continues the gradient exactly; for a flat panel it is
    // one colour, which is the "single colour of the edge areas"
    // alternative. It never invents detail and never distorts
    // the menu's own layout.
    lb_edge_fill(native, out, output_width, output_height, extra, extra_y);
}

// ---- LbRowPool ------------------------------------------------------------
//
// The persistent workers lb_parallel_rows hands row bands to. Built once, on
// first use, with the worker count that function already chose; torn down at
// static destruction.
//
// The partition is deliberately identical to the spawn-per-call code it
// replaces -- band = ceil(rows / n), the calling thread takes band 0, worker i
// takes band i -- so the same rows are computed by the same arithmetic and the
// output cannot move. LB_POOL=0 runs the old path for exactly that comparison.
//
// Each wake is one generation counter tick. Workers wait on it, take their
// band, and decrement a pending count; run() does band 0 itself and then waits
// for pending to reach zero, so no work is lost if a worker is slow to wake.
//
// docs/358: WITH CHUNKS, WAIT FOR THE ROWS, NOT FOR THE WORKERS. The pending
// count made the caller wait for every worker to wake and check in, including
// one the OS started after every chunk was already taken, which then did no
// work at all. So the chunks (docs/355) moved rows off a late thread but not
// the late thread's start off the frame. Now a chunked call claims rows from
// one word tagged with the call's generation and waits until every row is
// done; a worker that wakes late finds nothing left to claim, or a newer
// generation, and never touches a finished call's body. LB_ROWBAR=0 restores
// the pending-count barrier.
//
// The workers also run at the emulation thread's priority (LB_ROWPRIO=0
// leaves them at normal), because that thread waits for them. Measured
// in-run (LB_POOLAB=prio): at normal priority the worst composite in a block
// of 300 reached 4.7-7.9 ms quiet and 19-25 ms under load, as other threads
// took a worker's CPU mid-chunk; at high it stayed under 3.4 and 5.4 ms, and
// the loaded mean was lower in 11 of 11 pairs. The whole frame's tail did
// not measurably change either way (docs/358 s5). Measured, the barrier on
// its own was neutral, quiet and loaded; it is kept because it is exact.
extern "C" void lb_thread_priority_high(int on);
extern "C" void lb_render_thread_priority_high(int on);
class LbRowPool {
public:
    explicit LbRowPool(int workers) : n_(workers > 1 ? workers : 1) {
        for (int i = 1; i < n_; ++i)
            threads_.emplace_back([this, i] { worker(i); });
    }
    ~LbRowPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++gen_;
        }
        go_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    // docs/349: LB_ROWCHUNK=K hands the rows out K at a time from a shared
    // counter instead of one fixed band per thread. With fixed bands the
    // frame waits for the most expensive band -- the HUD rows, a dense
    // clump of sprites -- while the threads that drew plain ground sit idle.
    // Every row is still computed by the same body on one thread, and the
    // output does not depend on the partition (LB_THREADS=1, a single band,
    // is already the A/B that shows it), so only the timing can change.
    // 0 keeps the fixed bands.
    //
    // MEASURED, AND LEFT OFF -- RETRACTED (docs/355). That was 2560x1440, pan,
    // adaptive_ticks 4.416 ms with the fixed bands against 4.474 ms at
    // LB_ROWCHUNK=16: two separate runs, on a machine whose load moves the
    // frame by several times the effect. Interleaved in ONE run (LB_ROWAB,
    // 300 composites of each in turn, 13 pairs a run), the chunks win: at
    // 3440x1440 8-row chunks 2.95 ms against fixed bands 3.30 ms, 13 of 13
    // pairs; 16-row 2.95 against 3.13; 32-row 3.06 against 3.19; at
    // 1920x1080 16-row 1.90 against 2.12. Under load a row thread can start
    // up to 3 ms late (LB_ROWTIME), and with fixed bands the whole frame
    // waits for its band; with chunks the threads that did start take its
    // rows. 8-row chunks are therefore the DEFAULT; LB_ROWCHUNK=0 restores the
    // fixed bands. Output cannot depend on the partition -- every row is the
    // same body on one thread -- and since docs/352 no per-thread row state
    // outlives a call.
    static int row_chunk() {
        static const int v = [] {
            const char* s = std::getenv("LB_ROWCHUNK");
            if (!s) return 8;
            const int n = std::atoi(s);
            return n > 0 ? n : 0;
        }();
        return v;
    }
    // LB_ROWTIME=1 (docs/353): WHAT THE CALLER WAITS FOR. The caller does
    // band 0 and then waits for the slowest worker, so the composite costs
    // its slowest band plus however late that worker woke. A 3440x1440
    // profile spent ~10% of the main thread in kernel waits the rasteriser
    // was not responsible for (LB_3DWAIT: 0 ms at VCount144, <0.5 ms in
    // GetLine), and adaptive_ticks was 4.1 ms where ten threads should give
    // about one. Fixed bands only: with the chunked default (docs/355) set
    // LB_ROWCHUNK=0 to time them, or use LB_ROWAB to compare the two.
    static bool row_time() {
        static const bool on = std::getenv("LB_ROWTIME") != nullptr;
        return on;
    }
    using RowClock = std::chrono::steady_clock;
    static double ms_between(RowClock::time_point a, RowClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    // LB_ROWAB=K (docs/355): fixed bands and K-row chunks INTERLEAVED in one
    // run, 300 composites each in turn, timing the whole row phase of each.
    // Separate runs could not settle LB_ROWCHUNK on this machine: the load
    // moved emulation time 12.7-15 ms between runs, several times the effect
    // (docs/353 s6). Interleaved, both modes see the same load.
    static int row_ab() {
        static const int v = [] {
            const char* s = std::getenv("LB_ROWAB");
            const int n = s ? std::atoi(s) : 0;
            return n > 0 ? n : 0;
        }();
        return v;
    }
    // docs/358. The rows barrier and the raised priority are the defaults;
    // LB_ROWBAR=0 and LB_ROWPRIO=0 turn each off. LB_POOLAB=bar or =prio
    // interleaves one of them, off and on, 300 composites of each in turn,
    // and reports the row phase of each: mean, 95th percentile and worst.
    static bool row_bar_default() {
        static const bool on = [] {
            const char* s = std::getenv("LB_ROWBAR");
            return !(s && s[0] == '0');
        }();
        return on;
    }
    static int row_prio_default() {
        static const int on = [] {
            const char* s = std::getenv("LB_ROWPRIO");
            return (s && s[0] == '0') ? 0 : 1;
        }();
        return on;
    }
    // 0 off, 1 the barrier, 2 the workers' priority, 3 the 3D render
    // thread's priority (LB_RENDERPRIO, set in src/lb_prof.cpp).
    static int pool_ab() {
        static const int v = [] {
            const char* s = std::getenv("LB_POOLAB");
            if (!s) return 0;
            if (s[0] == 'b') return 1;   // bar
            if (s[0] == 'p') return 2;   // prio
            if (s[0] == 'r') return 3;   // render
            return 0;
        }();
        return v;
    }
    void run(int rows, const std::function<void(int, int)>& body) {
        if (rows <= 0) return;
        const int band = (rows + n_ - 1) / n_;
        const int ab = row_ab();
        const int ab_mode = ab ? (ab_calls_ / 300) & 1 : 0;   // 1 = chunked
        const RowClock::time_point ab_t0 = ab ? RowClock::now()
                                              : RowClock::time_point{};
        const int chunk = ab ? (ab_mode ? ab : 0) : row_chunk();
        const bool timed = row_time() && !chunk;
        const int pab = pool_ab();
        const int pab_mode = pab ? (pab_calls_ / 300) & 1 : 0;   // 1 = on
        const RowClock::time_point pab_t0 = pab ? RowClock::now()
                                                : RowClock::time_point{};
        const bool row_bar =
            chunk != 0 && (pab == 1 ? pab_mode == 1 : row_bar_default());
        const int prio = pab == 2 ? pab_mode : row_prio_default();
        if (pab) {
            // The whole frame: from the previous call's start to this one's,
            // charged to the previous call's setting (its composite is what
            // this frame's emulation ran after).
            if (pab_prev_mode_ >= 0)
                pab_frame_note(pab_prev_mode_, ms_between(pab_prev_t0_, pab_t0));
            pab_prev_t0_ = pab_t0;
            pab_prev_mode_ = pab_mode;
            if (pab == 3 && pab_render_set_ != pab_mode) {
                lb_render_thread_priority_high(pab_mode);
                pab_render_set_ = pab_mode;
            }
        }
        uint32_t tag = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            body_ = &body;
            rows_ = rows;
            band_ = band;
            chunk_ = chunk;
            row_bar_ = row_bar;
            prio_ = prio;
            next_.store(0, std::memory_order_relaxed);
            pending_ = 0;
            if (!row_bar)
                for (int i = 1; i < n_; ++i)
                    if (chunk || i * band < rows) ++pending_;
            if (timed) go_at_ = RowClock::now();
            ++gen_;
            tag = static_cast<uint32_t>(gen_);
            rows_done_.store(0, std::memory_order_relaxed);
            claim_.store(uint64_t{tag} << 32, std::memory_order_relaxed);
        }
        go_.notify_all();
        const RowClock::time_point t0 = timed ? RowClock::now()
                                              : RowClock::time_point{};
        if (row_bar)
            take_tagged(&body, tag, rows, chunk);
        else if (chunk)
            take_chunks(body);
        else
            body(0, band < rows ? band : rows);
        const RowClock::time_point t1 = timed ? RowClock::now()
                                              : RowClock::time_point{};
        std::unique_lock<std::mutex> lk(m_);
        if (row_bar)
            done_.wait(lk, [this, rows] {
                return rows_done_.load(std::memory_order_acquire) >= rows;
            });
        else
            done_.wait(lk, [this] { return pending_ == 0; });
        body_ = nullptr;
        if (timed) account(t0, t1, RowClock::now(), rows, band);
        if (pab) pool_ab_account(pab, pab_mode, pab_t0, rows);
        if (ab) {
            ab_ms_[ab_mode] += ms_between(ab_t0, RowClock::now());
            ++ab_n_[ab_mode];
            if (++ab_calls_ % 600 == 0) {
                std::fprintf(stderr,
                             "[lb_rowab] %d rows on %d threads: fixed bands "
                             "%.2f ms, %d-row chunks %.2f ms (300 composites "
                             "each, interleaved)\n",
                             rows, n_, ab_ms_[0] / ab_n_[0], ab,
                             ab_ms_[1] / ab_n_[1]);
                std::fflush(stderr);
                ab_ms_[0] = ab_ms_[1] = 0.0;
                ab_n_[0] = ab_n_[1] = 0;
            }
        }
    }
    int ab_calls_ = 0;
    double ab_ms_[2] = {0.0, 0.0};
    int ab_n_[2] = {0, 0};
    // docs/358, LB_POOLAB: the row phase of every call, binned by 0.1 ms per
    // setting, reported every 600 calls as mean, 95th percentile and worst.
    // The tail is the point: a late worker costs a few frames a second, not
    // the average one.
    void pool_ab_account(int pab, int mode, RowClock::time_point t0,
                         int rows) {
        const double ms = ms_between(t0, RowClock::now());
        pab_ms_[mode] += ms;
        ++pab_n_[mode];
        if (ms > pab_max_[mode]) pab_max_[mode] = ms;
        const int bin = ms < 0.0 ? 0 : static_cast<int>(ms * 10.0);
        ++pab_hist_[mode][bin < kPabBins ? bin : kPabBins - 1];
        if (++pab_calls_ % 600 != 0) return;
        double p95[2] = {0.0, 0.0};
        for (int m = 0; m < 2; ++m) {
            const int want = (pab_n_[m] * 95 + 99) / 100;
            int seen = 0;
            for (int b = 0; b < kPabBins; ++b) {
                seen += pab_hist_[m][b];
                if (seen >= want) { p95[m] = (b + 1) / 10.0; break; }
            }
        }
        double f95[2] = {0.0, 0.0};
        for (int m = 0; m < 2; ++m) {
            const int want = (pab_fn_[m] * 95 + 99) / 100;
            int seen = 0;
            for (int b = 0; b < kPabFrameBins; ++b) {
                seen += pab_fhist_[m][b];
                if (seen >= want) { f95[m] = (b + 1) / 10.0; break; }
            }
        }
        static const char* const kWhat[4] = {"", "barrier", "priority",
                                             "render thread"};
        static const char* const kOff[4] = {"", "all workers", "normal",
                                            "normal"};
        static const char* const kOn[4] = {"", "all rows", "high", "high"};
        std::fprintf(stderr,
                     "[lb_poolab] %s, %d rows on %d threads, 300 composites "
                     "each interleaved: %s mean %.3f p95 %.1f max %.2f ms | "
                     "%s mean %.3f p95 %.1f max %.2f ms; frame %s p95 %.1f "
                     "max %.1f, >20 ms %d | %s p95 %.1f max %.1f, >20 ms %d\n",
                     kWhat[pab], rows, n_,
                     kOff[pab], pab_n_[0] ? pab_ms_[0] / pab_n_[0] : 0.0,
                     p95[0], pab_max_[0],
                     kOn[pab], pab_n_[1] ? pab_ms_[1] / pab_n_[1] : 0.0,
                     p95[1], pab_max_[1],
                     kOff[pab], f95[0], pab_fmax_[0], pab_f20_[0],
                     kOn[pab], f95[1], pab_fmax_[1], pab_f20_[1]);
        std::fflush(stderr);
        for (int m = 0; m < 2; ++m) {
            pab_ms_[m] = 0.0;
            pab_n_[m] = 0;
            pab_max_[m] = 0.0;
            for (int b = 0; b < kPabBins; ++b) pab_hist_[m][b] = 0;
            pab_fn_[m] = 0;
            pab_fmax_[m] = 0.0;
            pab_f20_[m] = 0;
            for (int b = 0; b < kPabFrameBins; ++b) pab_fhist_[m][b] = 0;
        }
    }
    void pab_frame_note(int mode, double ms) {
        ++pab_fn_[mode];
        if (ms > pab_fmax_[mode]) pab_fmax_[mode] = ms;
        if (ms > 20.0) ++pab_f20_[mode];
        const int bin = ms < 0.0 ? 0 : static_cast<int>(ms * 10.0);
        ++pab_fhist_[mode][bin < kPabFrameBins ? bin : kPabFrameBins - 1];
    }
    static constexpr int kPabBins = 200;   // 0.1 ms bins, 20 ms and over last
    static constexpr int kPabFrameBins = 500;   // 0.1 ms bins to 50 ms
    int pab_calls_ = 0;
    double pab_ms_[2] = {0.0, 0.0};
    int pab_n_[2] = {0, 0};
    double pab_max_[2] = {0.0, 0.0};
    int pab_hist_[2][kPabBins] = {};
    int pab_fn_[2] = {0, 0};
    double pab_fmax_[2] = {0.0, 0.0};
    int pab_f20_[2] = {0, 0};
    int pab_fhist_[2][kPabFrameBins] = {};
    RowClock::time_point pab_prev_t0_{};
    int pab_prev_mode_ = -1;
    int pab_render_set_ = -1;
private:
    void take_chunks(const std::function<void(int, int)>& fn) {
        for (;;) {
            const int lo = next_.fetch_add(chunk_, std::memory_order_relaxed);
            if (lo >= rows_) return;
            fn(lo, lo + chunk_ < rows_ ? lo + chunk_ : rows_);
        }
    }
    // docs/358. Claims chunk after chunk of call `tag` and returns when none
    // are left or a newer call owns the word. `fn` is only called for a chunk
    // claimed in its own call, which cannot have returned: it waits for every
    // row, and a claimed row is not done until `fn` has run it. So a worker
    // holding a finished call's (or a null) body never calls it.
    void take_tagged(const std::function<void(int, int)>* fn, uint32_t tag,
                     int rows, int chunk) {
        for (;;) {
            uint64_t v = claim_.load(std::memory_order_acquire);
            int lo;
            for (;;) {
                if (static_cast<uint32_t>(v >> 32) != tag) return;
                lo = static_cast<int>(static_cast<uint32_t>(v));
                if (lo >= rows) return;
                if (claim_.compare_exchange_weak(
                        v, v + static_cast<uint64_t>(chunk),
                        std::memory_order_acq_rel, std::memory_order_acquire))
                    break;
            }
            const int hi = lo + chunk < rows ? lo + chunk : rows;
            (*fn)(lo, hi);
            if (rows_done_.fetch_add(hi - lo, std::memory_order_acq_rel) +
                    (hi - lo) >= rows) {
                // Under the lock, so the caller cannot test the count and
                // then miss this wake.
                std::lock_guard<std::mutex> lk(m_);
                done_.notify_one();
            }
        }
    }
    void worker(int index) {
        uint64_t seen = 0;
        int my_prio = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            go_.wait(lk, [&] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
            const std::function<void(int, int)>* const fn = body_;
            const int want_prio = prio_;
            if (chunk_) {
                const bool row_bar = row_bar_;
                const uint32_t tag = static_cast<uint32_t>(gen_);
                const int rows = rows_, chunk = chunk_;
                lk.unlock();
                if (want_prio != my_prio) {
                    lb_thread_priority_high(want_prio);
                    my_prio = want_prio;
                }
                if (row_bar) {
                    take_tagged(fn, tag, rows, chunk);
                    continue;
                }
                take_chunks(*fn);
                lk.lock();
                if (--pending_ == 0) done_.notify_one();
                continue;
            }
            const int lo = index * band_;
            if (lo >= rows_) continue;
            const int hi = (lo + band_ < rows_) ? (lo + band_) : rows_;
            const RowClock::time_point go = go_at_;
            lk.unlock();
            if (want_prio != my_prio) {
                lb_thread_priority_high(want_prio);
                my_prio = want_prio;
            }
            const bool timed = row_time() && index < kTimedSlots;
            const RowClock::time_point w0 = timed ? RowClock::now()
                                                  : RowClock::time_point{};
            (*fn)(lo, hi);
            if (timed) {
                // Written before the lock is retaken, read by the caller
                // after its wait takes the same lock.
                start_ms_[index] = ms_between(go, w0);
                band_ms_[index] = ms_between(w0, RowClock::now());
            }
            lk.lock();
            if (--pending_ == 0) done_.notify_one();
        }
    }
    void account(RowClock::time_point t0, RowClock::time_point t1,
                 RowClock::time_point t2, int rows, int band) {
        double slowest = 0.0, latest = 0.0, sum = 0.0;
        int nb = 0;
        for (int i = 1; i < n_ && i < kTimedSlots; ++i) {
            if (i * band >= rows) continue;
            sum += band_ms_[i];
            ++nb;
            if (start_ms_[i] + band_ms_[i] > slowest)
                slowest = start_ms_[i] + band_ms_[i];
            if (start_ms_[i] > latest) latest = start_ms_[i];
        }
        acc_caller_ += ms_between(t0, t1);
        acc_wait_ += ms_between(t1, t2);
        acc_mean_ += nb ? sum / nb : 0.0;
        acc_done_ += slowest;
        acc_late_ += latest;
        if (++acc_n_ < 300) return;
        const double n = acc_n_;
        std::fprintf(stderr,
                     "[lb_rowtime] 300 composites, %d rows on %d threads: "
                     "caller's band %.2f ms, then waited %.2f ms; worker band "
                     "mean %.2f ms; last worker done %.2f ms after the wake "
                     "(latest start %.2f ms)\n",
                     rows, n_, acc_caller_ / n, acc_wait_ / n, acc_mean_ / n,
                     acc_done_ / n, acc_late_ / n);
        std::fflush(stderr);
        acc_caller_ = acc_wait_ = acc_mean_ = acc_done_ = acc_late_ = 0.0;
        acc_n_ = 0;
    }
    static constexpr int kTimedSlots = 64;
    RowClock::time_point go_at_{};
    double start_ms_[kTimedSlots] = {};
    double band_ms_[kTimedSlots] = {};
    double acc_caller_ = 0.0, acc_wait_ = 0.0, acc_mean_ = 0.0;
    double acc_done_ = 0.0, acc_late_ = 0.0;
    int acc_n_ = 0;
    const int n_;
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable go_, done_;
    const std::function<void(int, int)>* body_ = nullptr;
    int rows_ = 0, band_ = 0, pending_ = 0, chunk_ = 0;
    std::atomic<int> next_{0};
    uint64_t gen_ = 0;
    bool stop_ = false;
    // docs/358: the call's generation (high 32 bits) and next unclaimed row
    // (low 32), and how many rows of it are finished.
    std::atomic<uint64_t> claim_{0};
    std::atomic<int> rows_done_{0};
    bool row_bar_ = false;
    int prio_ = 0;
};

// ---- lb_fast_row: the widened field's per-pixel loop, specialised ----------
//
// docs/354. At 3440x1440 the widened composite costs ~36 ns a pixel on each of
// its ten threads, ~36 ms of CPU a frame (docs/353 s3), and most of that is
// the general per-pixel loop in lb_adaptive_fb_inner's row body, not the
// decode. Its disassembly says why: the row body is a [&] lambda, and a store
// through `dst` may alias any captured int, so output_width, hud_bg_count,
// mbmode and mb are each reloaded through the closure on every pixel; and
// every layer is pushed through an insertion into top/below whether or not it
// can still matter.
//
// This computes the SAME pixel under the conditions it checks for, with every
// input a plain value:
//
//   * The general loop keeps, of the layers it pushes, the two with the
//     smallest (priority, order), the base pixel filling in below them. Every
//     key here is distinct -- BG1..BG3 are orders 2..4, the 3D layer is order
//     1, the base is priority 4 order 5 -- so walking the layers in that order
//     and stopping at the second opaque one gives the same top and below.
//   * top and below carry exactly what the general loop's Pixels would, and
//     compose()'s own logic finishes the pixel -- inlined, for the alpha-0
//     pixels that are all this path ever sees -- with the same blend(),
//     blend5(), brighten()/darken() and to_rgb32().
//
// Not taken when OBJ is on (a sprite pixel carries its own alpha and
// priority), when the HD surfaces are being written (they want the stack
// WITHOUT the 3D layer), or when the host terrain supplies the base: the
// general loop handles those, unchanged.
//
// LB_FASTROW=0 turns it off. LB_FASTROW=check runs BOTH paths on every row
// and counts every pixel on which they differ -- the proof, and the guard
// against the general loop gaining a feature this one lacks.
int lb_fastrow_mode() {
    static const int mode = [] {
        const char* v = std::getenv("LB_FASTROW");
        if (!v) return 1;
        if (v[0] == '0') return 0;
        if (std::strcmp(v, "check") == 0) return 2;
        return 1;
    }();
    return mode;
}

struct LbFastLayer {
    const uint16_t* color;   // nullptr for the 3D layer
    int width;               // decoded width; <= 256 means hardware columns
    uint8_t prio, order, target;
};

void lb_fast_row(const Unit& u, int width, int extra, bool hud_anchor,
                 int hud_center_left, int hud_center_right,
                 const BgLine* lines, size_t line_count,
                 const uint32_t* line3d, uint8_t prio3d,
                 const uint8_t* cover, Pixel backdrop, uint32_t mbmode,
                 uint32_t mb, uint8_t* layer_row, uint32_t* __restrict dst) {
    LbFastLayer layer[4];
    int n = 0;
    for (size_t i = 0; i < line_count && n < 3; ++i)
        layer[n++] = {lines[i].color.data(), lines[i].decoded_width,
                      lines[i].prio, lines[i].order, lines[i].target};
    layer[n++] = {nullptr, 0, prio3d, 1, 0x01u};
    for (int i = 1; i < n; ++i) {            // insertion sort, four at most
        const LbFastLayer t = layer[i];
        int j = i;
        while (j > 0 && (t.prio < layer[j - 1].prio ||
                         (t.prio == layer[j - 1].prio &&
                          t.order < layer[j - 1].order))) {
            layer[j] = layer[j - 1];
            --j;
        }
        layer[j] = t;
    }
    // compose(), for the pixels this path can see. Every candidate is built
    // with Pixel::alpha 0 -- BG and 3D pixels always are, and so is the
    // backdrop; the host-terrain base (alpha 3) is excluded by the caller --
    // so compose()'s semi-transparent-OBJ branch never applies, and what is
    // left reads four registers that are fixed for the row.
    const uint32_t bldcnt = u.bldcnt;
    const uint32_t effect = (bldcnt >> 6) & 3u;
    const uint32_t eva = u.eva, evb = u.evb, evy = u.evy;
    const uint32_t base_color = backdrop.color;
    const uint8_t base_target = backdrop.target, base_order = backdrop.order;
    for (int x = 0; x < width; ++x) {
        // The general loop's hud_x, both ways it can be set.
        int hud_x = -1;
        if (hud_anchor) {
            if (x < hud_center_left) hud_x = x;
            else if (x >= extra + hud_center_left &&
                     x < extra + hud_center_right) hud_x = x - extra;
            else if (x >= width - (256 - hud_center_right))
                hud_x = x - (width - 256);
        } else if (x >= extra && x < extra + 256) {
            hud_x = x - extra;
        }
        // top/below as the general loop's Pixels would carry them: colour,
        // BLDCNT target bit, 3D alpha (0 = not 3D) and order.
        uint32_t top_c = base_color, below_c = base_color;
        uint8_t top_t = base_target, below_t = base_target;
        uint8_t top_a5 = 0, top_o = base_order;
        int found = 0;
        for (int k = 0; k < n; ++k) {
            const LbFastLayer& l = layer[k];
            uint32_t pc;
            uint8_t pt, pa5 = 0, po;
            if (!l.color) {
                const uint32_t c3 = line3d[x];
                const uint8_t a3 = static_cast<uint8_t>((c3 >> 24) & 0x1Fu);
                if (!a3) continue;
                pc = c3 & 0x003F3F3Fu;
                pt = 0x01u;
                pa5 = a3;
                po = 1;
            } else {
                const int src = l.width > 256 ? x : hud_x;
                if (src < 0 || src >= l.width) continue;
                const uint16_t c = l.color[src];
                if (!(c & 0x8000u)) continue;
                if (cover && cover[x] && (l.target & 0x0Cu)) continue;
                pc = rgb6(static_cast<uint16_t>(c & 0x7FFFu));
                pt = l.target;
                po = l.order;
            }
            if (found++ == 0) {
                top_c = pc;
                top_t = pt;
                top_a5 = pa5;
                top_o = po;
            } else {
                below_c = pc;
                below_t = pt;
                break;
            }
        }
        uint32_t color = top_c;
        const uint32_t target2 = static_cast<uint32_t>(below_t) << 8;
        if (top_a5) {
            if (bldcnt & target2) color = blend5(top_c, below_c, top_a5);
            else if (bldcnt & top_t) {
                if (effect == 2u) color = brighten(top_c, evy);
                else if (effect == 3u) color = darken(top_c, evy);
            }
        } else if (bldcnt & top_t) {
            if (effect == 1u) {
                if (bldcnt & target2) color = blend(top_c, below_c, eva, evb);
            } else if (effect == 2u) {
                color = brighten(top_c, evy);
            } else if (effect == 3u) {
                color = darken(top_c, evy);
            }
        }
        if (mbmode == 1u) color = brighten(color, mb, 0u);
        else if (mbmode == 2u) color = darken(color, mb, 15u);
        if (layer_row) layer_row[x] = static_cast<uint8_t>(top_o + 1);
        dst[x] = to_rgb32(color);
    }
}

// LB_FASTROW=check: the fast row goes here, the general loop into the frame,
// and the two are compared row by row.
uint32_t* lb_fast_scratch(int width) {
    thread_local std::vector<uint32_t> row;
    if (static_cast<int>(row.size()) < width) row.resize(width);
    return row.data();
}
std::atomic<uint64_t> g_fast_rows{0}, g_fast_diff_rows{0}, g_fast_diff_px{0};
std::atomic<uint64_t> g_fast_px{0}, g_fast_ns{0}, g_general_ns{0};
// The two paths run back to back on the same thread for the same row, so
// their times compare like with like whatever else the machine is doing.
void lb_fast_check_row(int y, const uint32_t* general, const uint32_t* fast,
                       int width, std::chrono::steady_clock::time_point t0,
                       std::chrono::steady_clock::time_point t1,
                       std::chrono::steady_clock::time_point t2) {
    int diff = 0, first = -1;
    for (int x = 0; x < width; ++x)
        if (general[x] != fast[x]) {
            if (first < 0) first = x;
            ++diff;
        }
    g_fast_px += static_cast<uint64_t>(width);
    g_fast_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_general_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    const uint64_t rows = ++g_fast_rows;
    if (diff) {
        ++g_fast_diff_rows;
        g_fast_diff_px += static_cast<uint64_t>(diff);
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr,
                         "[lb_fastrow] MISMATCH row %d: %d px, first at x=%d "
                         "(general %08X, fast %08X)\n",
                         y, diff, first, (unsigned)general[first],
                         (unsigned)fast[first]);
    }
    if (rows % 200000u == 0u) {
        const double px = static_cast<double>(g_fast_px.load());
        std::fprintf(stderr,
                     "[lb_fastrow] check: %llu rows compared, %llu differ "
                     "(%llu px); fast %.2f ns/px, general %.2f ns/px\n",
                     (unsigned long long)rows,
                     (unsigned long long)g_fast_diff_rows.load(),
                     (unsigned long long)g_fast_diff_px.load(),
                     px > 0 ? g_fast_ns.load() / px : 0.0,
                     px > 0 ? g_general_ns.load() / px : 0.0);
    }
}

// ---- lb_parallel_rows -----------------------------------------------------
//
// Split a row range across worker threads.
//
// WHY THE COMPOSITOR IS THE THING WORTH THREADING. Measured, twice: emu_ticks
// is 19.7 ms and does not move when the surface grows 2.5x, so the emulator
// core and the 3D rasteriser are not what a bigger window costs. The whole cost
// is nds_gpu2d_adaptive_framebuffer's per-row loop, and it is exactly linear --
// 1.343 ms over 524k pixels, 6.054 ms over 1.33M, about 4.5 ns a pixel. There
// is no hotspot inside it to remove; it is ~30 operations on each of 1.33M
// pixels. That shape is what threading is for.
//
// WHY IT IS SAFE. Each row reads state that is fixed for the frame -- dispcnt,
// the palette, OAM, VRAM, lb_widemap's tables -- and writes only its own row of
// the output. Three things were not per-row, and all three are handled in the
// same change as this:
//
//   * the per-row scratch (obj, the sky/black-run arrays, the decoded BG lines)
//     is declared INSIDE the body, so each worker gets its own set on its own
//     stack. Deliberately not thread_local: `obj[x]` is read in the per-pixel
//     loop, and a TLS access there would hand back part of what threading wins.
//   * lb_hud_scan is hoisted out and completes before any row starts. It writes
//     g_boxes/g_count, which every row then only reads.
//   * lb_hud_shift_line's per-row scratch in lb_hud.cpp is thread_local, and
//     lb_hud_cover_row reads it back on the same thread and the same row.
//
// LB_THREADS=N overrides the worker count. LB_THREADS=1 forces the original
// single-threaded path, which is the A/B that shows the picture is unchanged.
template <typename F>
void lb_parallel_rows(int rows, F body) {
    static int workers = -1;
    if (workers < 0) {
        workers = 0;
        if (const char* v = std::getenv("LB_THREADS")) workers = std::atoi(v);
        if (workers <= 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            workers = hw ? static_cast<int>(hw) : 1;
            // Leave two cores for the emulator core and the 3D render thread,
            // and take the rest.
            //
            // This used to be a hard cap of 6, on the reasoning that those two
            // threads want cores and that the gain flattens early on a loop
            // this memory-heavy. The first half was measured when emu_ticks was
            // 19.7 ms and the core really was the bigger claim on the machine.
            // docs/263 took emu_ticks to 10.7 ms, and the cap then cost frames.
            //
            // Swept on a 12-core machine, 946x516 surface, adaptive_ticks (which
            // is reproducible to three decimals here, unlike fps):
            //
            //    LB_THREADS   1     6     8    10    12
            //    adaptive  8.269 3.523 2.999 2.727 2.760 ms
            //    emu      10.320 10.56 10.77 10.80 10.73 ms
            //
            // The knee is 10, and 12 is worse than 10 -- that is the emulator
            // thread being crowded off. emu_ticks does NOT degrade from 6 to
            // 10, which is what retires the old rationale: the cores those two
            // threads want are still there at hw-2.
            //
            // LB_THREADS=N overrides. LB_THREADS=1 forces the single-threaded
            // path, which on this machine measures 48.6 fps with drain_ticks at
            // 0.002 ms -- no slack left at all -- and is the A/B that shows the
            // picture is unchanged.
            workers = (workers > 3) ? (workers - 2) : 1;
        }
    }
    // Not worth a thread on a short surface: at the DS's own 192 rows the whole
    // loop is a fraction of a millisecond and the joins would dominate.
    if (workers <= 1 || rows < 256) {
        body(0, rows);
        return;
    }

    // A POOL, NOT NINE THREADS A FRAME.
    //
    // This function used to construct and join `workers - 1` std::threads on
    // EVERY call. At the measured default of hardware_concurrency - 2 that is
    // nine CreateThread/join pairs per composited frame, inside adaptive_ticks,
    // at roughly 25-60 us each on Windows.
    //
    // It also explains a number that sat unexplained in docs/264: the worker
    // sweep gave 8.269 ms at one thread against 2.727 at ten -- a 3.03x
    // speedup where the ideal is 10x. Some of that gap is memory bandwidth and
    // row imbalance, but the spawn cost is in there and it is the part that is
    // free to remove.
    //
    // The calling thread still takes band 0 and the partition is unchanged, so
    // every row is computed by the same arithmetic as before and the output is
    // identical. LB_POOL=0 restores the spawn-per-call path below, which is the
    // A/B that shows the pixels did not move; LB_THREADS=1 still forces the
    // fully serial path and never reaches here.
    static int pool_on = -1;
    if (pool_on < 0) {
        const char* v = std::getenv("LB_POOL");
        pool_on = (v && v[0] == '0') ? 0 : 1;
    }
    if (pool_on) {
        static LbRowPool shared(workers);
        shared.run(rows, [&body](int lo, int hi) { body(lo, hi); });
        return;
    }
    const int band = (rows + workers - 1) / workers;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    for (int w = 1; w < workers; ++w) {
        const int lo = w * band;
        if (lo >= rows) break;
        const int hi = (lo + band < rows) ? (lo + band) : rows;
        pool.emplace_back([&body, lo, hi]() { body(lo, hi); });
    }
    // This thread takes the first band rather than idling on the join.
    body(0, band < rows ? band : rows);
    for (std::thread& t : pool) t.join();
}

// ---- lb_radial_fill (docs/389) ---------------------------------------------
//
// A MENU'S BACKDROP, CONTINUED AS ITS OWN GRADIENT: the menus consistent, and
// the gradient natural toward the centre instead of the edges stretched.
//
// LEGO Battles' menus sit on a radial gradient, bright in the middle and dark
// at the edges, with a fine dither over it. lb_edge_fill carried each row's
// edge pixel sideways and then the top and bottom rows up and down, which
// turned that gradient into a cross: horizontal streaks beside the picture,
// vertical ones above and below it, and flat blocks in the corners.
//
// Here every pixel outside the picture takes the colour where the ray from
// the picture's centre leaves it -- the border smoothed along its own length
// (a 49-pixel box, so the dither does not streak) -- darkened with the
// distance past the border at the rate the picture itself darkens over its
// outer 24 pixels (none when it does not). The result is one gradient,
// centred on the window, that meets the picture without a seam. It invents
// no detail and moves nothing inside the picture.
//
// The ray geometry depends only on the surface and the rect, so it is built
// once per size; each frame smooths the 892-pixel border, and recomputes the
// outside only when that border or the rate changed (a fade does, a still
// menu does not), in parallel rows. Each engine keeps its own cache.
// LB_MENUFILL=edge restores the edge carry.
namespace lb_radial {
struct Cache {
    int w = -1, h = -1, rx = 0, ry = 0, rw = 0, rh = 0;
    std::vector<uint16_t> idx;      // outside pixel -> border index; inside 0xFFFF
    std::vector<uint16_t> beyond;   // past the border along the ray, quarter px
    int max_beyond = 0;
    std::vector<uint32_t> img;      // the outside, as last computed
    uint64_t sig = 0;
    bool have = false;
};
Cache g_cache[2];

bool enabled() {
    static const int on = [] {
        const char* v = std::getenv("LB_MENUFILL");
        return (v && v[0] == 'e') ? 0 : 1;
    }();
    return on != 0;
}

// The border as one loop: top left to right, right top to bottom, bottom
// right to left, left bottom to top.
int loop_len(int rw, int rh) { return 2 * rw + 2 * (rh - 2); }
void loop_xy(int i, int rw, int rh, int* x, int* y) {
    if (i < rw) { *x = i; *y = 0; return; }
    i -= rw;
    if (i < rh - 1) { *x = rw - 1; *y = 1 + i; return; }
    i -= rh - 1;
    if (i < rw - 1) { *x = rw - 2 - i; *y = rh - 1; return; }
    i -= rw - 1;
    *x = 0; *y = rh - 2 - i;
}
int loop_index(int x, int y, int rw, int rh) {
    if (y == 0) return x;
    if (x == rw - 1) return rw + (y - 1);
    if (y == rh - 1) return rw + (rh - 1) + (rw - 2 - x);
    return rw + (rh - 1) + (rw - 1) + (rh - 2 - y);
}

void build(Cache& c, int w, int h, int rx, int ry, int rw, int rh) {
    c.w = w; c.h = h; c.rx = rx; c.ry = ry; c.rw = rw; c.rh = rh;
    c.have = false;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    c.idx.assign(n, 0xFFFFu);
    c.beyond.assign(n, 0u);
    c.img.assign(n, 0xFF000000u);
    c.max_beyond = 0;
    const double cx = rx + (rw - 1) * 0.5, cy = ry + (rh - 1) * 0.5;
    const double left = rx - 0.5, right = rx + rw - 0.5;
    const double top = ry - 0.5, bottom = ry + rh - 0.5;
    std::vector<int> row_max(static_cast<size_t>(h), 0);
    lb_parallel_rows(h, [&](int lo, int hi) {
    for (int y = lo; y < hi; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) continue;
            const double dx = x - cx, dy = y - cy;
            double t = 1e30;
            if (dx > 0) t = std::min(t, (right - cx) / dx);
            if (dx < 0) t = std::min(t, (left - cx) / dx);
            if (dy > 0) t = std::min(t, (bottom - cy) / dy);
            if (dy < 0) t = std::min(t, (top - cy) / dy);
            int qx = static_cast<int>(cx + dx * t - rx + 0.5);
            int qy = static_cast<int>(cy + dy * t - ry + 0.5);
            qx = std::max(0, std::min(rw - 1, qx));
            qy = std::max(0, std::min(rh - 1, qy));
            if (qy != 0 && qy != rh - 1 && qx != 0 && qx != rw - 1)
                qx = (qx < rw - 1 - qx) ? 0 : rw - 1;
            const double len = __builtin_sqrt(dx * dx + dy * dy);
            const double past = t < 1.0 ? len * (1.0 - t) : 0.0;
            const int b = std::min(65535, static_cast<int>(past * 4.0 + 0.5));
            const size_t k = static_cast<size_t>(y) * w + x;
            c.idx[k] = static_cast<uint16_t>(loop_index(qx, qy, rw, rh));
            c.beyond[k] = static_cast<uint16_t>(b);
            row_max[static_cast<size_t>(y)] =
                std::max(row_max[static_cast<size_t>(y)], b);
        }
    }
    });
    for (int m : row_max) c.max_beyond = std::max(c.max_beyond, m);
}

// Mean of r+g+b over the ring `inset` pixels inside the rect.
double ring_luma(const uint32_t* out, int w, int rx, int ry, int rw, int rh,
                 int inset) {
    double s = 0.0;
    long n = 0;
    auto add = [&](int x, int y) {
        const uint32_t p = out[static_cast<size_t>(ry + y) * w + rx + x];
        s += (p & 0xFFu) + ((p >> 8) & 0xFFu) + ((p >> 16) & 0xFFu);
        ++n;
    };
    for (int x = inset; x < rw - inset; ++x) { add(x, inset); add(x, rh - 1 - inset); }
    for (int y = inset + 1; y < rh - 1 - inset; ++y) { add(inset, y); add(rw - 1 - inset, y); }
    return n ? s / n : 0.0;
}
}  // namespace lb_radial

// Fill every pixel of `out` (w x h) outside the rect (rx, ry, rw, rh), whose
// pixels are already drawn. Returns false (nothing written) when disabled or
// the rect is too small to measure, so the caller keeps its old fill.
bool lb_radial_fill(uint32_t* out, int w, int h, int rx, int ry, int rw,
                    int rh) {
    using namespace lb_radial;
    if (!enabled() || rw < 64 || rh < 64 || rx < 0 || ry < 0 ||
        rx + rw > w || ry + rh > h)
        return false;
    if (rw == w && rh == h) return true;           // nothing outside
    Cache& c = g_cache[lb_fill_engine & 1];
    if (c.w != w || c.h != h || c.rx != rx || c.ry != ry || c.rw != rw ||
        c.rh != rh)
        build(c, w, h, rx, ry, rw, rh);
    const int n = loop_len(rw, rh);
    // The border, smoothed along the loop (circular, radius 24).
    static thread_local std::vector<uint32_t> sr, sg, sb;
    sr.assign(static_cast<size_t>(n), 0u);
    sg.assign(static_cast<size_t>(n), 0u);
    sb.assign(static_cast<size_t>(n), 0u);
    {
        std::vector<uint32_t> raw(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            int x = 0, y = 0;
            loop_xy(i, rw, rh, &x, &y);
            raw[static_cast<size_t>(i)] =
                out[static_cast<size_t>(ry + y) * w + rx + x];
        }
        const int r = 24, m = 2 * r + 1;
        uint32_t ar = 0, ag = 0, ab = 0;
        for (int k = -r; k <= r; ++k) {
            const uint32_t p = raw[static_cast<size_t>(((k % n) + n) % n)];
            ar += (p >> 16) & 0xFFu; ag += (p >> 8) & 0xFFu; ab += p & 0xFFu;
        }
        for (int i = 0; i < n; ++i) {
            sr[static_cast<size_t>(i)] = ar / m;
            sg[static_cast<size_t>(i)] = ag / m;
            sb[static_cast<size_t>(i)] = ab / m;
            const uint32_t o = raw[static_cast<size_t>(((i - r) % n + n) % n)];
            const uint32_t q = raw[static_cast<size_t>((i + r + 1) % n)];
            ar += ((q >> 16) & 0xFFu) - ((o >> 16) & 0xFFu);
            ag += ((q >> 8) & 0xFFu) - ((o >> 8) & 0xFFu);
            ab += (q & 0xFFu) - (o & 0xFFu);
        }
    }
    // How fast the picture darkens toward its border, per pixel.
    const double outer = ring_luma(out, w, rx, ry, rw, rh, 0);
    const double inner = ring_luma(out, w, rx, ry, rw, rh, 24);
    double rate = (inner - outer) / 24.0 / (outer > 1.0 ? outer : 1.0);
    if (rate < 0.0) rate = 0.0;
    if (rate > 0.03) rate = 0.03;
    const uint32_t rate_q = static_cast<uint32_t>(rate * 1e6);
    uint64_t sig = 1469598103934665603ull ^ rate_q;
    for (int i = 0; i < n; ++i) {
        sig ^= (sr[static_cast<size_t>(i)] << 16) |
               (sg[static_cast<size_t>(i)] << 8) | sb[static_cast<size_t>(i)];
        sig *= 1099511628211ull;
    }
    if (!c.have || sig != c.sig) {
        std::vector<uint16_t> fall(static_cast<size_t>(c.max_beyond) + 1u);
        for (int b = 0; b <= c.max_beyond; ++b)
            fall[static_cast<size_t>(b)] = static_cast<uint16_t>(
                256.0 * __builtin_exp(-static_cast<double>(rate_q) * 1e-6 *
                                      (b * 0.25)) + 0.5);
        const uint32_t* const R = sr.data();
        const uint32_t* const G = sg.data();
        const uint32_t* const B = sb.data();
        lb_parallel_rows(h, [&](int lo, int hi) {
            for (int y = lo; y < hi; ++y) {
                const size_t row = static_cast<size_t>(y) * w;
                for (int x = 0; x < w; ++x) {
                    const uint16_t i = c.idx[row + x];
                    if (i == 0xFFFFu) continue;
                    const uint32_t f = fall[c.beyond[row + x]];
                    c.img[row + x] = 0xFF000000u | (((R[i] * f) >> 8) << 16) |
                                     (((G[i] * f) >> 8) << 8) |
                                     ((B[i] * f) >> 8);
                }
            }
        });
        c.sig = sig;
        c.have = true;
        // LB_RADIALLOG=1: one line per new geometry, the first 24.
        static const bool log_on = std::getenv("LB_RADIALLOG") != nullptr;
        static int said = 0;
        static int last_key = -1;
        const int key = (rx << 20) ^ (ry << 10) ^ w ^ (h << 5) ^ (lb_fill_engine << 30);
        if (log_on && said < 24 && key != last_key) {
            ++said;
            last_key = key;
            size_t mapped = 0;
            for (uint16_t v : c.idx) mapped += v != 0xFFFFu;
            const size_t probe = static_cast<size_t>(ry > 0 ? ry - 1 : 0) * w +
                                 static_cast<size_t>(rx + rw / 2);
            std::fprintf(stderr, "[lb_radial] engine %d %dx%d rect %d,%d %dx%d: "
                         "rate %.4f, border[0] %u,%u,%u, mapped %zu, above "
                         "centre %08X, max_beyond %d\n", lb_fill_engine, w, h,
                         rx, ry, rw, rh, rate, sr[0], sg[0], sb[0], mapped,
                         (unsigned)c.img[probe], c.max_beyond);
        }
    }
    // The outside from the cache; the rect's own pixels stay as drawn.
    for (int y = 0; y < h; ++y) {
        uint32_t* const dst = out + static_cast<size_t>(y) * w;
        const uint32_t* const src = c.img.data() + static_cast<size_t>(y) * w;
        if (y < ry || y >= ry + rh) {
            std::copy_n(src, w, dst);
        } else {
            std::copy_n(src, rx, dst);
            std::copy_n(src + rx + rw, w - rx - rw, dst + rx + rw);
        }
    }
    return true;
}

// ---- lb_threadcheck -------------------------------------------------------
//
// LB_THREADCHECK=1: composite the SAME frame again, single-threaded, and
// compare it with what the workers produced.
//
// TWO SEPARATE RUNS CANNOT ANSWER THIS, and trying it first wasted a pair of
// them. The game is a live simulation and the size sweep photographs whichever
// frame it happens to reach, so a threaded run is at a different point in the
// battle from a single-threaded one: 24 of 25 captures differed, and not one of
// those differences was evidence about the compositor. Units had moved.
//
// Same frame, same guest state, both code paths, memcmp. That is the only
// comparison that carries information, and a race shows up as a nonzero pixel
// count on some frames rather than all of them -- which is why the counters
// below are per-frame and cumulative rather than a single verdict.
template <typename F>
void lb_threadcheck(uint32_t* out, int width, int height, F& body) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_THREADCHECK") ? 1 : 0;
    if (!on) return;

    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    static std::vector<uint32_t> threaded;
    threaded.assign(out, out + n);

    body(0, height);                 // the same rows again, on this thread

    size_t bad = 0;
    for (size_t i = 0; i < n; ++i)
        if (threaded[i] != out[i]) ++bad;

    static unsigned frames = 0, bad_frames = 0;
    static size_t worst = 0;
    ++frames;
    if (bad) {
        ++bad_frames;
        if (bad > worst) worst = bad;
    }
    if ((frames % 120u) == 0u) {
        std::fprintf(stderr,
                     "[lb_threadcheck] %dx%d: %u frames, %u differed "
                     "(MUST be 0), worst %zu of %zu pixels\n",
                     width, height, frames, bad_frames, worst, n);
        std::fflush(stderr);
    }
}

// ---- LB_LAYERDUMP ---------------------------------------------------------
//
// WHICH LAYER WON EACH PIXEL. The band beside the guest's picture draws flat
// grass, and the measurements have now ruled out the obvious causes: the
// terrain table holds real tiles there (11 zero cells of 192 in the visible
// band, and the period-3 pattern is just metatile structure), the camera model
// is accepted (model_bad_frames stops at 73, the boot settle), nothing is
// declined out of table, and nothing is being blanked. So the data is right and
// something between the table and the screen is throwing it away.
//
// Pixel colour cannot answer that -- grass drawn by BG1 and grass drawn by a
// stale overlay are the same green. The layer identity can: Pixel::order is
// exactly "equal-priority order: OBJ, BG0, BG1, BG2, BG3", so recording the
// winner per pixel says whether the band is terrain that is wrong, or terrain
// that is covered.
//
// LB_LAYERDUMP=N writes a greyscale map every N frames (1 means every 240).
// Records order + 1. THE BACKDROP IS NOT 0: it is a real Pixel with
// valid=true and order 5 (see the backdrop construction above), so a
// pixel with nothing drawn on it records 6, not 0. An earlier version of
// this comment claimed 0, and the analysis tools inherited it -- which is
// why "20% of the surface draws nothing" sat in every dump unnoticed,
// labelled only as an unknown layer id.
std::vector<uint8_t> g_lb_layer;
bool g_lb_layer_on = false;

int lb_layerdump_every() {
    static int every = -1;
    if (every < 0) {
        const char* v = std::getenv("LB_LAYERDUMP");
        every = v ? std::atoi(v) : 0;
        if (every == 1) every = 240;
    }
    return every;
}

extern "C" unsigned lb_dump_frame_tick(void);   /* src/lb_widemap.cpp */
extern "C" unsigned lb_dump_frame_get(void);
extern "C" unsigned lb_demo_frame(void);        /* src/lb_demo.cpp */

// LB_DRIFTLOG wants the layer map, but NOT the files.
//
// g_lb_layer_on gates FILLING the array; lb_layer_write's `every` gates
// WRITING the PGMs. Keeping those two separate lets the drift probe read which
// layer won each pixel on every frame without putting 488 KB on disk per
// frame.
//
// That separation is the whole point. Pairing the probe against dump FILES by
// frame number does not work: the layer dumps land on frames divisible by
// `every` and the entity log fires every 60 frames at its own phase, so with
// LB_LAYERDUMP=10 the two sets were disjoint (0 of 55 paired) and with =3 they
// were disjoint again. Reading the array in the same breath as everything else
// removes the pairing question instead of re-tuning cadences until two clocks
// happen to line up.
int lb_driftlog_on() {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_DRIFTLOG") ? 1 : 0;
    return on;
}

// The band/camera values the composite decided on, parked so they can be
// printed on the SAME LINE as the 3D extent below.
//
// They are computed at the top of the composite, before lb_layer_begin() ticks
// the shared frame counter, so anything printed there carries a frame number
// one behind the dumps. Joining the two produced lines claiming DIFFERENT
// SURFACE WIDTHS for the same frame (1712 against 1572) -- two clocks read as
// one, which is the same mistake that has wasted three measurements in this
// hunt. Parking the values and emitting them once, after the rows, makes the
// question unaskable.
struct LbDriftParked {
    bool have = false;
    int w = 0, extra = 0, band_left = 0, snap = 0, render = 0;
    int fill_cam = 0, cam_tx = 0, field_x = 0, field_y = 0;
    unsigned dispcnt = 0;
};
LbDriftParked g_lb_drift_parked;

void lb_drift_park(int w, int extra, int band_left, int snap, int render,
                   int fill_cam, int cam_tx, unsigned dispcnt,
                   int field_x, int field_y) {
    g_lb_drift_parked = {true, w, extra, band_left, snap, render,
                         fill_cam, cam_tx, field_x, field_y, dispcnt};
}

// Where the 3D layer -- which is what draws the units and the base; the layer
// histogram counts BG0/3D 2,361 px against BG1 203,256 and OBJ 0 -- actually
// landed this frame, stamped with the SHARED frame counter so it can be paired
// with [lb_ent] exactly rather than approximately.
void lb_drift_3d_extent(int width, int height) {
    if (!lb_driftlog_on() || !g_lb_layer_on) return;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (g_lb_layer.size() != n) return;
    int minx = width, maxx = -1, miny = height, maxy = -1;
    long count = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = g_lb_layer.data() + (size_t)y * width;
        for (int x = 0; x < width; ++x) {
            // THE ARRAY HOLDS (order + 1). The *40 that the layer histogram
            // documents is applied later, when the PGM is written (see
            // lb_layer_write), to spread the ids across a visible grey range.
            // Testing for 80 here -- the FILE encoding -- read zero 3D pixels
            // on all 3,564 frames at every width, while the dump files from the
            // same build plainly contained 2,361. A uniform zero is a broken
            // instrument, not a measurement. BG0/3D is order 1, so: 2.
            if (row[x] != 2u) continue;
            ++count;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    const LbDriftParked& p = g_lb_drift_parked;
    // The centred band this surface WOULD have if nothing clamped it. A
    // 3440x1440 window gives a ~1720 surface against a 1536-px-wide map, so
    // the extension asks for nearly the whole map and the clamp is in play far
    // more there than at 946. Printing both makes "ground on the clamped band,
    // units on the centred one" checkable directly instead of inferred.
    const int centre = (p.w > 256) ? (p.w - 256) / 2 : 0;
    std::fprintf(stderr,
                 "[lb_drift3d] f=%-6u w=%d h=%d px3d=%ld x=%d..%d y=%d..%d"
                 " | pw=%d extra=%d band_left=%d centre=%d snap=%d render=%d"
                 " fill_cam=%d cam_tx=%d fieldcam=%d,%d dispcnt=%08X\n",
                 lb_dump_frame_get(), width, height, count,
                 count ? minx : -1, maxx, count ? miny : -1, maxy,
                 p.have ? p.w : -1, p.extra, p.band_left, centre,
                 p.snap, p.render, p.fill_cam, p.cam_tx,
                 p.field_x, p.field_y, p.dispcnt);
}

void lb_layer_begin(int width, int height) {
    // Tick the SHARED dump counter once per composited frame, so the panel's
    // whole-map dump and this one number the same frame the same way. They used
    // to count separately and the audit paired a composite with another frame's
    // camera rect.
    lb_dump_frame_tick();
    g_lb_layer_on = lb_layerdump_every() > 0 || lb_driftlog_on();
    if (!g_lb_layer_on) return;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (g_lb_layer.size() != n) g_lb_layer.assign(n, 0u);
}

// The frame counter the map dump aligns against, so the composited picture and
// the whole-map render being compared are the same moment.
int g_lb_dump_frame = 0;

void lb_layer_write(const uint32_t* out_px, int width, int height) {
    const int every = lb_layerdump_every();
    if (every <= 0) return;
    const int me = (int)lb_dump_frame_get();   // the SHARED counter
    g_lb_dump_frame = me;
    if ((me % every) != 0) return;

    // THE COMPOSITED PICTURE ITSELF, beside the layer map.
    //
    // The goal: compare full-map renders against the composite on various
    // freeplay maps, with and without fog. The battlefield panel already
    // renders the WHOLE level from the grid in RAM, and the panel knows the
    // map rect the guest surface covers, so cropping that render to the rect
    // and diffing it against this buffer answers "does the widened band show
    // the RIGHT ground" as a number instead of an impression -- restricted to
    // the pixels where the terrain layer actually won, since the map render
    // carries no fog, units or HUD.
    if (out_px) {
        char cpath[300];
        const char* cdir = std::getenv("LB_DUMP_ADAPTIVE");
        std::snprintf(cpath, sizeof cpath, "%s%slb_comp-%dx%d-%d.ppm",
                      cdir ? cdir : ".", cdir ? "/" : "", width, height, me);
        if (std::FILE* cf = std::fopen(cpath, "wb")) {
            std::fprintf(cf, "P6\n%d %d\n255\n", width, height);
            std::vector<uint8_t> row(static_cast<size_t>(width) * 3u);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const uint32_t v = out_px[static_cast<size_t>(y) * width + x];
                    row[static_cast<size_t>(x) * 3u + 0u] = uint8_t(v >> 16);
                    row[static_cast<size_t>(x) * 3u + 1u] = uint8_t(v >> 8);
                    row[static_cast<size_t>(x) * 3u + 2u] = uint8_t(v);
                }
                std::fwrite(row.data(), 1, row.size(), cf);
            }
            std::fclose(cf);
        }
    }
    char path[300];
    const char* dir = std::getenv("LB_DUMP_ADAPTIVE");
    std::snprintf(path, sizeof path, "%s%slb_layers-%dx%d-%d.pgm",
                  dir ? dir : ".", dir ? "/" : "", width, height, me);
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P5\n%d %d\n255\n", width, height);
    std::vector<uint8_t> row(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            row[static_cast<size_t>(x)] = static_cast<uint8_t>(
                g_lb_layer[static_cast<size_t>(y) * width + x] * 40u);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    // WITH THE REPLAY'S OWN FRAME. `me` counts engine-A composites, and a
    // composite that returns early (a native-size or unsupported frame) does
    // not tick it, so it is not by itself the emulated moment. A replay keys
    // its input on lb_demo_frame(), and two runs whose dumps share a number
    // are the same moment only if this agrees too (docs/352 s4).
    std::fprintf(stderr, "[lb_layerdump] wrote %s (demo frame %u)\n", path,
                 lb_demo_frame());
    std::fflush(stderr);
}
