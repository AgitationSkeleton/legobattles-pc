/* legobattles-decomp docs/434: THE 3D LAYER AT N TIMES THE SURFACE, composited.

   Included into gpu2d.cpp's anonymous namespace, after compose() and the
   blend helpers it uses (apply_hooks.py, stanza 0zzzzzzzzzzzzz-433).

   The compositor resolves the 2D stack at the surface's own size. With "3D
   resolution" above Native it also keeps, for every surface pixel, the top two
   layers WITHOUT the 3D layer (the g_hd_* arrays docs/325's HD emit already
   wrote for the GPU presenter), and the hi-res partner renderer
   (GPU3D_Soft.cpp, lb_hires_soft.inc) has drawn the 3D layer N times as wide
   and tall. This puts the two together: for every pixel of the N-times image,
   the 3D sample is inserted into the stack at its own priority and the pixel
   finished by compose(), exactly as the surface pixel was. Where the 3D layer
   is not involved the N x N block is the surface pixel repeated.

   A surface pixel keeps the plain repeat when the picture the compositor
   used there is not the raster's own: a HUD element the port moved to a
   window corner, a reshifted row. Those are native-resolution by
   construction, and the repeat is what the surface shows. */

extern "C" int nds_gpu3d_hi_scale(void);
extern "C" int nds_gpu3d_hi_active(void);
extern "C" void nds_gpu3d_hi_wait(void);
extern "C" const uint32_t* nds_gpu3d_hi_rows(const uint32_t* line,
                                             int* stride, int* width,
                                             int* n, const uint8_t** mask);

std::vector<uint32_t> g_hi_out;
bool g_hi_valid = false;
int g_hi_w = 0, g_hi_h = 0, g_hi_n = 1;
std::atomic<unsigned> g_hi_rows_hi{0}, g_hi_rows_native{0};
std::atomic<unsigned> g_hi_px_3d{0}, g_hi_frame_3d{0};

/* The scale to composite at this frame, and the buffer for it. 1: not this
   frame (off, or the image would be past the limits). */
int lb_hi_alloc(int w, int h, int want) {
    if (want < 2) return 1;
    const long hw = (long)w * want, hh = (long)h * want;
    if (hw > 6144 || hh > 3072 || hw * hh > 12000000L) return 1;
    const size_t need = (size_t)hw * (size_t)hh;
    if (g_hi_out.size() != need) g_hi_out.assign(need, 0xFF000000u);
    return want;
}

inline Pixel lb_hi_pixel(uint32_t color, uint32_t meta) {
    Pixel p;
    p.color = color;
    p.target = static_cast<uint8_t>(meta & 0xFFu);
    p.alpha = static_cast<uint8_t>((meta >> 8) & 0xFFu);
    p.priority = static_cast<uint8_t>((meta >> 16) & 0xFFu);
    p.order = static_cast<uint8_t>((meta >> 24) & 0xFFu);
    p.valid = true;
    return p;
}

/* One surface row y: writes its N output rows into out. dst is the finished
   surface row, line3d the 3D row the compositor used, raw the raster's own
   row (equal pointers when nothing moved), topPx/belowPx this row's pairs of
   (colour, meta) from the HD emit.

   ONLY A SPECIAL'S MODEL GETS HI-RES SAMPLES. The hi-res raster draws every
   polygon (so a minifig in front of a ship still hides it), but its mask says
   which pixels belong to a special; a surface pixel whose N x N block holds
   none of them is the surface pixel repeated, exactly as Native shows it. In a
   block that does: a special's sample is composited at its own priority, a
   sample with no 3D layer at all shows the stack without it, and any other 3D
   sample keeps the surface pixel. */
void lb_hi_row(const Unit& u, int y, int W, int N, const uint32_t* dst,
               const uint32_t* line3d, const uint32_t* raw, uint8_t prio3d,
               uint32_t mbmode, uint32_t mb, const uint32_t* topPx,
               const uint32_t* belowPx, uint32_t* out) {
    const size_t HW = static_cast<size_t>(W) * static_cast<size_t>(N);
    uint32_t* const o0 = out + static_cast<size_t>(y) * N * HW;
    int stride = 0, hw = 0, hn = 0;
    const uint8_t* mask0 = nullptr;
    const uint32_t* const hi0 =
        raw ? nds_gpu3d_hi_rows(raw, &stride, &hw, &hn, &mask0) : nullptr;
    if (!hi0 || !mask0 || hn != N || hw < static_cast<int>(HW)) {
        /* The raster has no hi rows for this one: the surface, repeated. */
        for (int x = 0; x < W; ++x) {
            const uint32_t v = dst[x];
            uint32_t* const o = o0 + static_cast<size_t>(x) * N;
            for (int j = 0; j < N; ++j) o[j] = v;
        }
        for (int k = 1; k < N; ++k)
            std::memcpy(o0 + static_cast<size_t>(k) * HW, o0,
                        HW * sizeof(uint32_t));
        g_hi_rows_native.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_hi_rows_hi.fetch_add(1, std::memory_order_relaxed);
    const bool moved = line3d != raw;
    auto ahead = [](const Pixel& a, const Pixel& b) {
        return a.priority < b.priority ||
               (a.priority == b.priority && a.order < b.order);
    };
    /* Which surface pixels have a special's sample in their block. */
    std::array<uint8_t, kMaxAdaptiveWidth> spec;
    bool anySpec = false;
    for (int x = 0; x < W; ++x) {
        uint8_t s = 0;
        if (!(moved && line3d[x] != raw[x])) {
            for (int k = 0; k < N && !s; ++k) {
                const uint8_t* mk = mask0 + static_cast<size_t>(k) * stride +
                                    static_cast<size_t>(x) * N;
                for (int j = 0; j < N; ++j) s |= mk[j];
            }
        }
        spec[x] = s;
        anySpec = anySpec || s;
    }
    if (!anySpec) {
        for (int x = 0; x < W; ++x) {
            const uint32_t v = dst[x];
            uint32_t* const o = o0 + static_cast<size_t>(x) * N;
            for (int j = 0; j < N; ++j) o[j] = v;
        }
        for (int k = 1; k < N; ++k)
            std::memcpy(o0 + static_cast<size_t>(k) * HW, o0,
                        HW * sizeof(uint32_t));
        return;
    }
    unsigned px3d = 0;
    for (int k = 0; k < N; ++k) {
        const uint32_t* const hrow = hi0 + static_cast<size_t>(k) * stride;
        const uint8_t* const mrow = mask0 + static_cast<size_t>(k) * stride;
        uint32_t* const o = o0 + static_cast<size_t>(k) * HW;
        for (int x = 0; x < W; ++x) {
            uint32_t* const ox = o + static_cast<size_t>(x) * N;
            if (!spec[x]) {
                const uint32_t v = dst[x];
                for (int j = 0; j < N; ++j) ox[j] = v;
                continue;
            }
            const uint32_t* const hx = hrow + static_cast<size_t>(x) * N;
            const uint8_t* const mx = mrow + static_cast<size_t>(x) * N;
            const size_t pi = static_cast<size_t>(x) * 2u;
            for (int j = 0; j < N; ++j) {
                const uint32_t c3 = hx[j];
                const uint8_t a3 = static_cast<uint8_t>((c3 >> 24) & 0x1Fu);
                if (!mx[j] && a3) {   /* some other 3D: the surface pixel */
                    ox[j] = dst[x];
                    continue;
                }
                Pixel top = lb_hi_pixel(topPx[pi], topPx[pi + 1]);
                Pixel below = lb_hi_pixel(belowPx[pi], belowPx[pi + 1]);
                if (a3) {
                    const Pixel p3{c3 & 0x003F3F3Fu, 0x01u, 0, prio3d, 1,
                                   true, a3};
                    if (ahead(p3, top)) {
                        below = top;
                        top = p3;
                    } else if (ahead(p3, below)) {
                        below = p3;
                    }
                    ++px3d;
                }
                uint32_t color = compose(u, top, below);
                if (mbmode == 1u)
                    color = brighten(color, mb, 0u);
                else if (mbmode == 2u)
                    color = darken(color, mb, 15u);
                ox[j] = to_rgb32(color);
            }
        }
    }
    g_hi_px_3d.fetch_add(px3d, std::memory_order_relaxed);
    g_hi_frame_3d.fetch_add(px3d, std::memory_order_relaxed);
}

/* LB_HIRES_DUMP=<dir>: the finished N-times picture and the surface it came
   from, the same frame, as PPMs. LB_HIRES_EVERY=k picks every k-th, up to
   LB_HIRES_MAX files (default 1). */
void lb_hi_ppm(const char* path, const uint32_t* px, int w, int h) {
    FILE* fh = std::fopen(path, "wb");
    if (!fh) return;
    std::fprintf(fh, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> row(static_cast<size_t>(w) * 3u);
    for (int y = 0; y < h; ++y) {
        const uint32_t* s = px + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = static_cast<unsigned char>((s[x] >> 16) & 0xFFu);
            row[x * 3 + 1] = static_cast<unsigned char>((s[x] >> 8) & 0xFFu);
            row[x * 3 + 2] = static_cast<unsigned char>(s[x] & 0xFFu);
        }
        std::fwrite(row.data(), 1, row.size(), fh);
    }
    std::fclose(fh);
}

void lb_hi_frame_done(const uint32_t* native, int W, int H, int N) {
    g_hi_valid = true;
    g_hi_w = W * N;
    g_hi_h = H * N;
    g_hi_n = N;
    static const char* dir = std::getenv("LB_HIRES_DUMP");
    static const int every = [] {
        const char* v = std::getenv("LB_HIRES_EVERY");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? n : 60;
    }();
    static const int maxf = [] {
        const char* v = std::getenv("LB_HIRES_MAX");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? n : 1;
    }();
    static unsigned tick = 0;
    static int written = 0;
    static const bool log = std::getenv("LB_HIRES_LOG") != nullptr;
    ++tick;
    const unsigned f3d = g_hi_frame_3d.exchange(0);
    static const unsigned min3d = [] {
        const char* v = std::getenv("LB_HIRES_MIN3D");
        return v ? (unsigned)std::atoi(v) : 0u;
    }();
    if (log && (tick % 300u) == 0u) {
        std::fprintf(stderr,
                     "[lb_hires] %ux%u surface, %dx -> %dx%d: %u rows hi, "
                     "%u native, %u 3D samples (300 frames)\n",
                     (unsigned)W, (unsigned)H, N, W * N, H * N,
                     g_hi_rows_hi.exchange(0), g_hi_rows_native.exchange(0),
                     g_hi_px_3d.exchange(0));
        std::fflush(stderr);
    }
    if (dir && written < maxf && (min3d == 0 || f3d >= min3d) &&
        (tick % static_cast<unsigned>(every)) == 0u) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/hires-%d-%dx%d.ppm", dir,
                      written, W * N, H * N);
        lb_hi_ppm(path, g_hi_out.data(), W * N, H * N);
        std::snprintf(path, sizeof path, "%s/surface-%d-%dx%d.ppm", dir,
                      written, W, H);
        lb_hi_ppm(path, native, W, H);
        ++written;
    }
}
