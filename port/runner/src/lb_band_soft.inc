/* legobattles-decomp docs/484: THE 3D FRAME RASTERISED IN ROW BANDS.

   Included inside namespace melonDS at the end of GPU3D_Soft.cpp.

   At a 3440x1440 window the widened raster is about 1720x720, and one
   render thread took ~4 ms over it. The DS starts the frame at VCount215,
   48 lines (~2 ms of emulation) before the composite needs it, so the main
   thread waited ~2 ms a frame in nds_gpu3d_wide_line (LB_COMPTIME).

   WHY BANDS ARE EXACT. A polygon's edge position on row y is a function of
   y and of which pair of vertices bounds the edge there: Slope::Setup at y
   computes dx = start + (y - y0) * Increment, and Step adds Increment, in
   integers, so setting an edge up directly at a band's first row gives the
   value stepping would have reached. The vertex walk in SetupPolygon*Edge
   is monotonic in y, so calling it at the band's first row lands on the
   same vertex pair the sequential walk holds there. Every other piece of
   per-row state is row-local -- except three, handled here:

   - the shadow stencil (two rows, carried across rows by PrevIsShadowMask):
     a frame with any shadow or shadow-mask polygon renders sequentially;
   - lb_RowSplit[y - 1], read when row y clears the HUD set's border: the
     whole table is computed before the bands start, with the same test;
   - the final pass (edge marking reads the depth and IDs of rows y +- 1):
     it runs as a second phase, after every row is rasterised. It reads only
     depth and the ID bits, which no final pass writes.

   Each band runs on a helper SoftRenderer whose buffer pointers alias this
   one's, so the per-row pointer swaps of the HUD split (lb_UseHudBuffers)
   and the stencil and edge-merge scratch stay per thread.

   LB_3DBANDS=N sets the thread count (1 = the sequential path).
   LB_3DBANDCHECK=1 renders every frame both ways and counts differences. */

namespace {
struct LbBandPool {
    std::mutex m;
    std::condition_variable go, done;
    unsigned gen = 0;
    int pending = 0;
    const std::function<void(int)>* job = nullptr;
    int n;
    explicit LbBandPool(int workers) : n(workers) {
        for (int i = 1; i < n; ++i)
            std::thread([this, i] { loop(i); }).detach();
    }
    void loop(int i) {
        unsigned seen = 0;
        for (;;) {
            const std::function<void(int)>* f;
            {
                std::unique_lock<std::mutex> lk(m);
                go.wait(lk, [&] { return gen != seen; });
                seen = gen;
                f = job;
            }
            (*f)(i);
            {
                std::lock_guard<std::mutex> lk(m);
                if (--pending == 0) done.notify_one();
            }
        }
    }
    void run(const std::function<void(int)>& f) {
        {
            std::lock_guard<std::mutex> lk(m);
            job = &f;
            pending = n - 1;
            ++gen;
        }
        go.notify_all();
        f(0);
        std::unique_lock<std::mutex> lk(m);
        done.wait(lk, [&] { return pending == 0; });
    }
};

int lb_band_threads() {
    static const int v = [] {
        if (const char* s = std::getenv("LB_3DBANDS")) {
            const int n = std::atoi(s);
            return n < 1 ? 1 : (n > 16 ? 16 : n);
        }
        return 4;
    }();
    return v;
}

LbBandPool& lb_band_pool() {
    static LbBandPool* p = new LbBandPool(lb_band_threads());   /* never freed: detached threads */
    return *p;
}

struct LbBandStats {
    unsigned frames = 0, banded = 0, shadow = 0, above = 0;
    unsigned checked = 0, differ_frames = 0;
    unsigned long long differ_px = 0;
} g_lb_band_stats;
}  // namespace

SoftRenderer::SoftRenderer(LbBandTag) noexcept
    : Renderer3D(false)
{
    Sema_RenderStart = Platform::Semaphore_Create();
    Sema_RenderDone = Platform::Semaphore_Create();
    Sema_ScanlineCount = Platform::Semaphore_Create();
    RenderThreadRunning = false;
    RenderThreadRendering = false;
    RenderThread = nullptr;
    lb_IsBand = true;
}

void SoftRenderer::lb_BandSync(const SoftRenderer& m, int j)
{
    MaxRenderWidth = m.MaxRenderWidth;
    ScanlineWidth = m.ScanlineWidth;
    MaxRenderHeight = m.MaxRenderHeight;
    NumScanlines = m.NumScanlines;
    BufferSize = m.BufferSize;
    FirstPixelOffset = m.FirstPixelOffset;
    RenderWidth = m.RenderWidth;
    RenderHeight = m.RenderHeight;
    ColorStore[0] = m.ColorStore[0];
    ColorStore[1] = m.ColorStore[1];
    ColorBuffer = m.ColorBuffer;
    DisplayedBuffer = m.DisplayedBuffer;
    lb_DepthStore = m.lb_DepthStore;
    lb_AttrStore = m.lb_AttrStore;
    DepthBuffer = m.DepthBuffer;
    AttrBuffer = m.AttrBuffer;
    for (int s = 0; s < 2; s++)
    {
        lb_HudColor[s] = m.lb_HudColor[s];
        lb_WorldOut[s] = m.lb_WorldOut[s];
        lb_RowSplit[s] = m.lb_RowSplit[s];
        lb_FrameSplit[s] = m.lb_FrameSplit[s];
        lb_HudX0[s] = m.lb_HudX0[s];
        lb_HudX1[s] = m.lb_HudX1[s];
        lb_FrameNoHud[s] = m.lb_FrameNoHud[s];
    }
    lb_HudDepth = m.lb_HudDepth;
    lb_HudAttr = m.lb_HudAttr;
    memcpy(lb_PolyHud, m.lb_PolyHud, (size_t)j);
    memcpy(lb_HudIdx, m.lb_HudIdx, (size_t)m.lb_HudCount * sizeof(u16));
    lb_HudCount = m.lb_HudCount;
    lb_SplitFrame = m.lb_SplitFrame;
    lb_Slot = m.lb_Slot;
    lb_ClearZ = m.lb_ClearZ;
    lb_ClearAttr = m.lb_ClearAttr;
    lb_HudZ = m.lb_HudZ;
    lb_EdgeSplit = m.lb_EdgeSplit;
    lb_EdgeExact = m.lb_EdgeExact;
    lb_ForceNoSplit = m.lb_ForceNoSplit;
    PrevIsShadowMask = false;
    const size_t st = (size_t)MaxRenderWidth * 2 + 16;
    if (lb_BandStencil.size() < st) lb_BandStencil.assign(st, 0);
    StencilBuffer = lb_BandStencil.data();
    const size_t el = (size_t)ScanlineWidth + 16;
    if (lb_BandEdge.size() < el) lb_BandEdge.assign(el, 0);
    lb_EdgeLine = lb_BandEdge.data();
    for (int i = 0; i < j; i++)
        PolygonList[i].PolyData = m.PolygonList[i].PolyData;
}

void SoftRenderer::lb_BandRaster(const GPU& gpu, const SoftRenderer& m, int j, s32 y0, s32 y1)
{
    for (int i = 0; i < j; i++)
    {
        const Polygon* p = m.PolygonList[i].PolyData;
        const bool flat = p->YTop == p->YBottom;
        if (p->YTop >= y1) continue;
        if (flat ? (p->YTop < y0) : (p->YBottom <= y0)) continue;
        RendererPolygon* rp = &PolygonList[i];
        *rp = m.PolygonList[i];
        if (!flat && y0 > p->YTop)
        {
            SetupPolygonLeftEdge(rp, y0);
            SetupPolygonRightEdge(rp, y0);
        }
    }
    PrevIsShadowMask = false;
    for (s32 y = y0; y < y1; y++)
        RenderScanline(gpu, y, j);
}

bool SoftRenderer::lb_RenderBanded(const GPU& gpu, bool threaded, Polygon** polygons, int npolys, int j)
{
    if (lb_IsHi || lb_IsBand || lb_BandNested) return false;
    const int K = lb_band_threads();
    if (K <= 1 || RenderWidth <= 256) return false;
    LbBandStats& st = g_lb_band_stats;
    ++st.frames;
    for (int i = 0; i < j; i++)
    {
        const Polygon* p = PolygonList[i].PolyData;
        if (p->IsShadowMask || p->IsShadow) { ++st.shadow; return false; }
        if (p->YTop < 0) { ++st.above; return false; }
    }
    ++st.banded;

    static const bool check = std::getenv("LB_3DBANDCHECK") != nullptr;
    std::vector<u32>* ref = nullptr;
    static std::vector<u32> ref_c, ref_w, ref_h;
    if (check)
    {
        /* The sequential picture first, kept; then the same frame again,
           banded, over freshly cleared buffers. */
        lb_BandNested = true;
        RenderPolygons(gpu, false, polygons, npolys);
        lb_BandNested = false;
        ref_c.assign(ColorStore[lb_Slot], ColorStore[lb_Slot] + BufferSize);
        if (lb_SplitFrame)
        {
            ref_w.assign(lb_WorldOut[lb_Slot], lb_WorldOut[lb_Slot] + BufferSize);
            ref_h.assign(lb_HudColor[lb_Slot], lb_HudColor[lb_Slot] + BufferSize);
        }
        ref = &ref_c;
        ClearBuffers(gpu);
        for (int i = 0; i < j; i++)
            SetupPolygon(&PolygonList[i], PolygonList[i].PolyData);
        memset(lb_RowSplit[lb_Slot], 0, NumScanlines);
    }

    /* lb_RowSplit, whole, before any band reads row y - 1 of it. */
    if (lb_SplitFrame)
    {
        const int lim = std::min(RenderHeight, NumScanlines);
        for (int k = 0; k < lb_HudCount; k++)
        {
            const Polygon* p = PolygonList[lb_HudIdx[k]].PolyData;
            const int a = std::max(0, (int)p->YTop);
            const int b = std::min(lim, (p->YTop == p->YBottom) ? (int)p->YTop + 1 : (int)p->YBottom);
            for (int y = a; y < b; y++) lb_RowSplit[lb_Slot][y] = 1;
        }
    }

    while ((int)lb_Bands.size() < K)
        lb_Bands.push_back(std::make_unique<SoftRenderer>(LbBandTag{}));
    for (int w = 0; w < K; w++) lb_Bands[w]->lb_BandSync(*this, j);

    const int H = RenderHeight;
    const int chunk = std::max(8, (H + 3 * K - 1) / (3 * K));
    std::atomic<int> next{0};
    const std::function<void(int)> raster = [&](int w) {
        SoftRenderer& h = *lb_Bands[w];
        for (;;)
        {
            const int y0 = next.fetch_add(1, std::memory_order_relaxed) * chunk;
            if (y0 >= H) break;
            h.lb_BandRaster(gpu, *this, j, y0, std::min(H, y0 + chunk));
        }
    };
    lb_band_pool().run(raster);

    if (lb_EdgeSplit && lb_RowSplit[lb_Slot][H - 1])
        lb_ClearHudRow(H);   /* docs/388: the border */

    next.store(0, std::memory_order_relaxed);
    const std::function<void(int)> finish = [&](int w) {
        SoftRenderer& h = *lb_Bands[w];
        for (;;)
        {
            const int y0 = next.fetch_add(1, std::memory_order_relaxed) * chunk;
            if (y0 >= H) break;
            const int y1 = std::min(H, y0 + chunk);
            for (s32 y = y0; y < y1; y++) h.lb_FinishRow(gpu.GPU3D, y);
        }
    };
    lb_band_pool().run(finish);

    if (ref)
    {
        unsigned long long d = 0;
        for (int y = 0; y < H; y++)
        {
            const int base = FirstPixelOffset + y * ScanlineWidth;
            for (int x = 0; x < RenderWidth; x++)
            {
                if (ColorStore[lb_Slot][base + x] != ref_c[base + x]) d++;
                if (lb_SplitFrame && lb_RowSplit[lb_Slot][y] &&
                    x >= lb_HudX0[lb_Slot] && x < lb_HudX1[lb_Slot] &&
                    (lb_WorldOut[lb_Slot][base + x] != ref_w[base + x] ||
                     lb_HudColor[lb_Slot][base + x] != ref_h[base + x]))
                    d++;
            }
        }
        ++st.checked;
        if (d) { ++st.differ_frames; st.differ_px += d; }
        if (st.checked % 120u == 1u || (d && st.differ_frames <= 8u))
            std::fprintf(stderr, "[lb_3dband] check: %u frames compared, %u differ (%llu px)%s\n",
                         st.checked, st.differ_frames, st.differ_px,
                         d ? " <- this frame" : "");
    }
    if (st.frames % 1800u == 0u)
        std::fprintf(stderr, "[lb_3dband] %u widened frames: %u banded (%d threads), %u sequential for shadows, %u for rows above the top\n",
                     st.frames, st.banded, K, st.shadow, st.above);

    if (threaded)
    {
        RenderedScanlines.store(MaxRenderHeight, std::memory_order_release);
        Platform::Semaphore_Post(Sema_ScanlineCount, H);
    }
    return true;
}
