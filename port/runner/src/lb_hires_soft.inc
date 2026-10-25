/* legobattles-decomp docs/434: THE 3D LAYER AT N TIMES THE SURFACE.

   Included at the very end of GPU3D_Soft.cpp, inside namespace melonDS
   (apply_hooks.py, stanza 0zzzzzzzzzzzzz-433).

   The native renderer is untouched: it draws the 3D layer at the guest
   surface's own size, every consumer (the compositor's HUD moves, the field
   locator, the pairing) reads that, and the simulation never sees anything
   else. "3D resolution" adds a SECOND SoftRenderer, the hi-res partner, owned
   by the native one. It draws the same polygon list a second time, into
   buffers N times as wide and tall, from vertex positions the geometry stage
   computed at N times the resolution (Vertex::lb_HiPos, GPU3D.cpp). Texture
   coordinates, depth, W, colours and every register are the native frame's,
   so the picture is the same picture with finer edges and finer shading.

   The partner mirrors the native renderer's colour slot: when the native one
   renders into slot s, so does the partner, and when the native one skips an
   identical frame, so does it. That is what lets the compositor find the hi
   rows of any native row it holds (lb_HiRows), live or the previous frame's.

   Nothing here runs unless "3D resolution" is above Native. */

bool SoftRenderer::lb_Alloc(int maxW, int maxH, bool hud)
{
    const int sw = maxW + 2, sh = maxH + 2;
    const size_t bs = (size_t)sw * (size_t)sh;
    /* colour 2 sets x 2, depth 2, attributes 2; the HUD's own colour,
       depth, attributes and the world-only rows only on the native one. */
    size_t words = bs * 8;
    if (hud) words += bs * 10;
    words += (size_t)sw + 16;
    u32* m = (u32*)std::calloc(words, sizeof(u32));
    const size_t bytes = (size_t)sh * 2 + (size_t)maxW * 2 + 16 +
                         (hud ? 0 : bs * 2);   /* + the specials' masks */
    u8* m8 = (u8*)std::calloc(bytes, 1);
    if (!m || !m8)
    {
        std::free(m);
        std::free(m8);
        return false;
    }
    lb_Mem.reset(m);
    lb_Mem8.reset(m8);
    MaxRenderWidth = maxW;
    MaxRenderHeight = maxH;
    ScanlineWidth = sw;
    NumScanlines = sh;
    BufferSize = (int)bs;
    FirstPixelOffset = sw + 1;

    u32* p = m;
    ColorStore[0] = p; p += bs * 2;
    ColorStore[1] = p; p += bs * 2;
    lb_DepthStore = p; p += bs * 2;
    lb_AttrStore = p; p += bs * 2;
    if (hud)
    {
        lb_HudColor[0] = p; p += bs * 2;
        lb_HudColor[1] = p; p += bs * 2;
        lb_HudDepth = p; p += bs * 2;
        lb_HudAttr = p; p += bs * 2;
        lb_WorldOut[0] = p; p += bs;
        lb_WorldOut[1] = p; p += bs;
    }
    else
    {
        lb_HudColor[0] = lb_HudColor[1] = nullptr;
        lb_HudDepth = lb_HudAttr = nullptr;
        lb_WorldOut[0] = lb_WorldOut[1] = nullptr;
    }
    lb_EdgeLine = p;
    lb_RowSplit[0] = m8;
    lb_RowSplit[1] = m8 + sh;
    StencilBuffer = m8 + (size_t)sh * 2;
    if (!hud)
    {
        lb_Spec[0] = m8 + (size_t)sh * 2 + (size_t)maxW * 2 + 16;
        lb_Spec[1] = lb_Spec[0] + bs;
    }
    else
    {
        lb_Spec[0] = lb_Spec[1] = nullptr;
    }

    ColorBuffer = ColorStore[0];
    DisplayedBuffer = ColorStore[0];
    ShownColor.store(ColorStore[0], std::memory_order_release);
    DepthBuffer = lb_DepthStore;
    AttrBuffer = lb_AttrStore;
    return true;
}

SoftRenderer::SoftRenderer(bool lbhi) noexcept
    : Renderer3D(false)
{
    Sema_RenderStart = Platform::Semaphore_Create();
    Sema_RenderDone = Platform::Semaphore_Create();
    Sema_ScanlineCount = Platform::Semaphore_Create();

    RenderThreadRunning = false;
    RenderThreadRendering = false;
    RenderThread = nullptr;
    lb_IsHi = lbhi;
    lb_ForceNoSplit = lbhi;   /* the partner draws the picture unsplit */
}

void SoftRenderer::lb_HiSetScale(int n, GPU& gpu)
{
    if (lb_IsHi) return;
    if (n < 2 || !Threaded)
    {
        if (lb_Hi) lb_Hi.reset();   /* stops its thread */
        lb_HiN = 1;
        return;
    }
    if (lb_Hi && lb_HiN == n) return;
    lb_Hi.reset();
    lb_HiN = n;
    std::unique_ptr<SoftRenderer> h = std::make_unique<SoftRenderer>(true);
    h->lb_HiN = n;
    h->Threaded = true;
    /* Set up, not enabled: SetThreaded would start a frame on buffers that
       do not exist yet. */
    h->SetupRenderThread(gpu);
    lb_Hi = std::move(h);
    std::fprintf(stderr, "[lb_hires] 3D at %dx the surface\n", n);
}

/* The hi-res twin of the frame the native renderer is about to draw. Main
   thread, inside RenderFrame, after FrameIdentical is known and before the
   native thread starts. slot is the colour slot the native frame will use. */
void SoftRenderer::lb_HiFrame(GPU& gpu, int slot)
{
    SoftRenderer* h = lb_Hi.get();
    if (!h) return;
    static const int kHiMaxW = 6144, kHiMaxH = 3072;
    static const long kHiMaxPixels = 12000000L;
    const int N = lb_HiN;
    const int hw = RenderWidth * N, hh = RenderHeight * N;
    h->lb_HiStarted = false;
    h->lb_WantOk = false;
    h->lb_HiReady = false;
    if (hw > kHiMaxW || hh > kHiMaxH || (long)hw * (long)hh > kHiMaxPixels)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            std::fprintf(stderr, "[lb_hires] %dx%d at %dx is past the "
                         "limit (6144x3072, 12M pixels): not drawn\n",
                         RenderWidth, RenderHeight, N);
        }
        h->lb_HiOk[0] = h->lb_HiOk[1] = false;
        return;
    }
    if (!h->lb_Mem || h->MaxRenderWidth != hw || h->MaxRenderHeight != hh)
    {
        /* The partner's thread is idle here: its last frame was waited for
           at VCount144 (or by the compositor). */
        if (!h->lb_Alloc(hw, hh, false))
        {
            h->lb_Mem.reset();
            h->lb_HiOk[0] = h->lb_HiOk[1] = false;
            return;
        }
        h->RenderWidth = hw;
        h->RenderHeight = hh;
        h->lb_HiOk[0] = h->lb_HiOk[1] = false;
        FrameIdentical = false;   /* both draw this frame */
    }
    if (FrameIdentical)
    {
        h->FrameIdentical = true;   /* the thread only reports done */
    }
    else
    {
        h->FrameIdentical = false;
        h->lb_HiOk[slot] = false;
        h->lb_SpecAny[slot] = false;
        if (h->lb_BuildPolys(gpu))
        {
            h->lb_PendingSlot = slot;
            h->lb_WantOk = true;
        }
        else
        {
            h->FrameIdentical = true;   /* a vertex without a hi position */
        }
    }
    h->lb_HiReady = true;
    h->lb_HiStarted = true;
    h->EnableRenderThread();
}

/* The polygon list again, each vertex at its hi-res position. */
bool SoftRenderer::lb_BuildPolys(GPU& gpu)
{
    GPU3D& g3 = gpu.GPU3D;
    const int n = (int)g3.RenderNumPolygons;
    if (lb_HiPoly.empty())
    {
        lb_HiPoly.resize(2048);
        lb_HiVert.resize(2048 * 10);
        lb_HiPolyPtr.resize(2048);
    }
    int nv = 0, np = 0;
    const int hw = RenderWidth, hh = RenderHeight;
    for (int i = 0; i < n && i < 2048; i++)
    {
        const Polygon* src = g3.RenderPolygonRAM[i];
        Polygon& dp = lb_HiPoly[np];
        dp = *src;
        if (!src->Degenerate)
        {
            const u32 nverts = src->NumVertices;
            if (nverts > 10 || nv + (int)nverts > (int)lb_HiVert.size())
                return false;
            s32 ytop = hh, ybot = 0, xtop = hw, xbot = 0;
            u32 vtop = 0, vbot = 0;
            for (u32 v = 0; v < nverts; v++)
            {
                const Vertex* sv = src->Vertices[v];
                if (sv->lb_HiScale != lb_HiN) return false;
                Vertex& hv = lb_HiVert[nv++];
                hv = *sv;
                hv.FinalPosition[0] = sv->lb_HiPos[0];
                hv.FinalPosition[1] = sv->lb_HiPos[1];
                dp.Vertices[v] = &hv;
                const s32 fx = hv.FinalPosition[0], fy = hv.FinalPosition[1];
                if (fy < ytop) { xtop = fx; ytop = fy; vtop = v; }
                if (fy > ybot || (fy == ybot && fx > xbot))
                {
                    xbot = fx; ybot = fy; vbot = v;
                }
            }
            dp.VTop = vtop; dp.VBottom = vbot;
            dp.YTop = ytop; dp.YBottom = ybot;
            dp.XTop = xtop; dp.XBottom = xbot;
        }
        lb_HiPolyPtr[np] = &dp;
        np++;
    }
    lb_HiNumPolys = np;
    return true;
}

/* Wait for the partner's frame, if one is in flight. Main thread. */
void SoftRenderer::lb_HiWaitDone(GPU& gpu)
{
    SoftRenderer* h = lb_Hi.get();
    if (!h || !h->lb_HiStarted) return;
    if (!h->RenderThreadRunning.load(std::memory_order_relaxed))
    {
        h->lb_HiStarted = false;
        return;
    }
    if (gpu.GPU3D.AbortFrame) return;
    Platform::Semaphore_Wait(h->Sema_RenderDone);
    h->lb_HiStarted = false;
}

/* The hi-res rows behind a native row the compositor holds: the first of
   the N rows that native row y covers, in the partner's buffer for the same
   slot. Null when the line is not one of the native renderer's own rows, the
   slot has no hi frame, or the scale is not the current one. */
const u32* SoftRenderer::lb_HiRows(const u32* line, int* stride, int* width,
                                   int* n, const u8** mask) const noexcept
{
    const SoftRenderer* h = lb_Hi.get();
    if (!h || !h->lb_Mem) return nullptr;
    for (int s = 0; s < 2; s++)
    {
        const u32* lo = ColorStore[s];
        if (line < lo || line >= lo + BufferSize) continue;
        const long off = (long)(line - lo) - FirstPixelOffset;
        if (off < 0 || (off % ScanlineWidth) != 0) return nullptr;
        const long row = off / ScanlineWidth;
        if (!h->lb_HiOk[s]) return nullptr;
        if ((row + 1) * lb_HiN > h->RenderHeight) return nullptr;
        if (h->RenderWidth != RenderWidth * lb_HiN) return nullptr;
        if (stride) *stride = h->ScanlineWidth;
        if (width) *width = h->RenderWidth;
        if (n) *n = lb_HiN;
        const size_t at = (size_t)h->FirstPixelOffset +
                          (size_t)row * lb_HiN * h->ScanlineWidth;
        if (mask) *mask = h->lb_Spec[s] + at;
        return h->ColorStore[s] + at;
    }
    return nullptr;
}

/* LB_POLYLOG=<period>: every period-th rendered frame, one line per polygon
   with the attributes that might tell a special's model from a billboard. */
void SoftRenderer::lb_PolyLog(const GPU& gpu, Polygon** polys, int n)
{
    static const int period = [] {
        const char* v = std::getenv("LB_POLYLOG");
        return v ? std::atoi(v) : 0;
    }();
    if (period <= 0) return;
    static int frame = 0;
    if ((frame++ % period) != 0) return;
    std::fprintf(stderr, "[lb_poly] frame %d DISP3DCNT %04X polygons %d\n",
                 frame, (unsigned)gpu.GPU3D.RenderDispCnt, n);
    for (int i = 0; i < n && i < 400; i++)
    {
        const Polygon* p = polys[i];
        s32 x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999;
        u32 zmin = 0xFFFFFFFFu;
        for (u32 v = 0; v < p->NumVertices && !p->Degenerate; v++)
        {
            x0 = std::min(x0, p->Vertices[v]->FinalPosition[0]);
            x1 = std::max(x1, p->Vertices[v]->FinalPosition[0]);
            y0 = std::min(y0, p->Vertices[v]->FinalPosition[1]);
            y1 = std::max(y1, p->Vertices[v]->FinalPosition[1]);
            zmin = std::min(zmin, (u32)p->FinalZ[v]);
        }
        std::fprintf(stderr, "[lb_poly]  %3d id %02X attr %08X light %X mode %u"
                     " alpha %2u fmt %u tex %08X pal %05X nv %u deg %d"
                     " box %d,%d %dx%d z %06X\n", i,
                     (unsigned)((p->Attr >> 24) & 0x3F), (unsigned)p->Attr,
                     (unsigned)(p->Attr & 0xF), (unsigned)((p->Attr >> 4) & 3),
                     (unsigned)((p->Attr >> 16) & 0x1F),
                     (unsigned)((p->TexParam >> 26) & 7), (unsigned)p->TexParam,
                     (unsigned)p->TexPalette, (unsigned)p->NumVertices,
                     p->Degenerate ? 1 : 0, x0, y0, x1 - x0, y1 - y0,
                     (unsigned)(zmin == 0xFFFFFFFFu ? 0 : zmin));
    }
    std::fflush(stderr);
}

/* The partner's row y: which pixels belong to a special's model. The
   cartridge draws the specials (ships, siege, the other Transport and Siege
   units, Building_Draw3D 0x0203B3AC) through NNS_G3dGlbPolygonAttr
   (0x020FDF50) with polygon ID 8, 0x10 or 0x18; every billboard, effect and
   in-field HUD quad has ID 0 (the only other caller, 0x0203A610, and the
   census of a battle's polygon list, docs/434). The pixel's opaque polygon
   ID is in the attribute buffer. */
void SoftRenderer::lb_SpecRow(s32 y)
{
    u8* m = lb_Spec[lb_Slot];
    if (!m || y < 0 || y >= RenderHeight) return;
    /* LB_HIRES_SPECID=<n>: a test aid, treat polygon ID n as the special (0
       sends every polygon through the hi-res path). */
    static const int testid = [] {
        const char* v = std::getenv("LB_HIRES_SPECID");
        return v ? std::atoi(v) : -1;
    }();
    const size_t base = (size_t)FirstPixelOffset + (size_t)y * ScanlineWidth;
    bool any = false;
    for (int x = 0; x < RenderWidth; x++)
    {
        const int id = (int)((AttrBuffer[base + x] >> 24) & 0x3F);
        m[base + x] = testid >= 0 ? (id == testid ? 1 : 0)
                                  : ((id == 8 || id == 0x10 || id == 0x18) ? 1 : 0);
        if (m[base + x]) any = true;
    }
    if (any) lb_SpecAny[lb_Slot] = true;
}
