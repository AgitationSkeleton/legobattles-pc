/* legobattles-decomp docs/434: "3D resolution", the frontend's half.

   Included into frontend.cpp's anonymous namespace after lb_render_field
   (apply_hooks.py, stanza 0zzzzzzzzzzzzz-433).

   lb_hires_prepare tells the 3D engine what scale to draw at, once a frame,
   before the frame's compositing. lb_hires_present_field draws the compositor's
   N-times picture instead of the surface when there is one for this frame;
   otherwise it returns false and the surface is presented as it always was. */

extern "C" int lb_setting_res3d(void);
extern "C" void nds_gpu3d_set_hi_scale(int n);
extern "C" bool nds_gpu2d_hi_frame(const uint32_t** px, int* w, int* h,
                                   int* n);

static void lb_hires_prepare() {
    const int n = lb_setting_enhanced() ? lb_setting_res3d() : 1;
    nds_gpu3d_set_hi_scale(n);
}

static SDL_Texture* lb_hi_tex = nullptr;
static SDL_Renderer* lb_hi_renderer = nullptr;
static int lb_hi_tw = 0, lb_hi_th = 0;
static SDL_Texture* lb_hi_sharp = nullptr;
static SDL_Renderer* lb_hi_sharp_renderer = nullptr;
static int lb_hi_sw = 0, lb_hi_sh = 0;

static bool lb_hires_present_field(FrontendPresentation& presentation,
                                   const SDL_Rect& destination, float scale) {
    const uint32_t* px = nullptr;
    int w = 0, h = 0, n = 1;
    if (!nds_gpu2d_hi_frame(&px, &w, &h, &n) || !px || w <= 0 || h <= 0 ||
        n < 2)
        return false;
    SDL_Renderer* r = presentation.renderers[1];
    if (!r) return false;
    if (!lb_hi_tex || lb_hi_renderer != r || lb_hi_tw != w ||
        lb_hi_th != h) {
        if (lb_hi_tex && lb_hi_renderer == r) SDL_DestroyTexture(lb_hi_tex);
        lb_hi_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, w, h);
        lb_hi_renderer = r;
        lb_hi_tw = w;
        lb_hi_th = h;
        if (!lb_hi_tex) {
            std::fprintf(stderr, "[lb_hires] texture %dx%d failed (%s)\n",
                         w, h, SDL_GetError());
            return false;
        }
        SDL_SetTextureBlendMode(lb_hi_tex, SDL_BLENDMODE_NONE);
        std::fprintf(stderr, "[lb_hires] presenting the %dx picture, "
                     "%dx%d\n", n, w, h);
    }
    SDL_UpdateTexture(lb_hi_tex, nullptr, px, w * (int)sizeof(uint32_t));
    /* Window pixels per hi pixel. At 1.0 the picture is shown as it is. */
    const float s2 = scale / (float)n;
    const int whole = (int)s2;
    const float frac = s2 - (float)whole;
    if (lb_present_sharp_on() && s2 > 1.0f && frac >= 0.05f &&
        frac <= 0.95f && w * (whole + 1) <= 16384 &&
        h * (whole + 1) <= 16384) {
        /* Between two whole multiples: an exact prescale, then one linear
           pass, as the surface itself is shown (docs/382). */
        const int k = whole + 1;
        const int tw = w * k, th = h * k;
        if (!lb_hi_sharp || lb_hi_sharp_renderer != r || lb_hi_sw != tw ||
            lb_hi_sh != th) {
            if (lb_hi_sharp && lb_hi_sharp_renderer == r)
                SDL_DestroyTexture(lb_hi_sharp);
            lb_hi_sharp = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, tw, th);
            lb_hi_sharp_renderer = r;
            lb_hi_sw = tw;
            lb_hi_sh = th;
            if (lb_hi_sharp)
                SDL_SetTextureScaleMode(lb_hi_sharp, SDL_ScaleModeLinear);
        }
        if (lb_hi_sharp) {
            SDL_SetTextureScaleMode(lb_hi_tex, SDL_ScaleModeNearest);
            SDL_SetRenderTarget(r, lb_hi_sharp);
            SDL_RenderCopy(r, lb_hi_tex, nullptr, nullptr);
            SDL_SetRenderTarget(r, nullptr);
            SDL_RenderCopy(r, lb_hi_sharp, nullptr, &destination);
            return true;
        }
    }
    SDL_SetTextureScaleMode(lb_hi_tex, s2 >= 0.999f ? SDL_ScaleModeNearest
                                                    : SDL_ScaleModeLinear);
    SDL_RenderCopy(r, lb_hi_tex, nullptr, &destination);
    return true;
}
