/* legobattles-decomp docs/506: Visuals 2.0, the frontend's half (phase 1, the
   cutscene theatre). Included into frontend.cpp after lb_hires_front.inc
   (apply_hooks.py, the Visuals 2.0 stanza).

   While the cartridge plays a film (lb_visuals.cpp decides), the main window
   becomes a theatre: the film, cleaned and doubled by Jaime A. Vives' Anime4K
   chain when that part is on, fills the window above a strip that holds the
   bottom screen -- the film's own pause / play / skip buttons. The field is
   simply drawn into that strip, so its click mapping (the rect passed to
   lb_overlay_set_field_rect) follows it without any other change. */

extern "C" void lb_visuals_top_frame(const uint32_t* top, int w);
extern "C" int lb_visuals_theatre_active(void);
extern "C" int lb_visuals_theatre_frame(uint32_t* dst, int cap, int* w, int* h, unsigned* gen);
extern "C" float lb_visuals_shake_px(float scale);   /* phase 2: the spell shake */

/* The film's buttons sit in a band across the middle of the bottom screen:
   guest rows 76..116 of its 192 (measured off a film's bottom screen). The
   strip shows that band, as large as it fits; the rest of the bottom screen
   is black there and lies under the film or off the window. */
static const int kLbThBandTop = 76, kLbThBandRows = 40;

static SDL_Rect lb_th_strip{0, 0, 0, 0};   /* this frame's strip, for the drawing */
static SDL_Rect lb_th_field{0, 0, 0, 0};   /* and the field's rect in it */
static int lb_th_win_w = 0;

/* The theatre's layout for a win_w x win_h window and a gw x gh guest
   surface: the field's rect and scale, and the film's rect. False when no
   film is playing, and the layout is left as it was. */
static bool lb_theatre_layout(int win_w, int win_h, int gw, int gh, SDL_Rect* field, float* scale,
                              SDL_Rect* film) {
    if (!lb_visuals_theatre_active() || gw <= 0 || gh <= 0 || win_w <= 0 || win_h <= 0) return false;
    int strip = win_h / 7;
    if (strip < 48) strip = 48;
    /* The guest's own 192 rows map onto the surface's gh. */
    const float rows = (float)gh / 192.0f;
    float fs = (float)strip / ((float)kLbThBandRows * rows);
    if ((float)gw * fs > (float)win_w) fs = (float)win_w / (float)gw;
    const int fw = (int)((float)gw * fs + 0.5f), fh = (int)((float)gh * fs + 0.5f);
    const int strip_top = win_h - strip;
    const int band_mid = (int)(((float)kLbThBandTop + (float)kLbThBandRows * 0.5f) * rows * fs + 0.5f);
    *field = SDL_Rect{(win_w - fw) / 2, strip_top + strip / 2 - band_mid, fw, fh};
    *scale = fs;
    float ms = (float)win_w / 256.0f;
    if ((float)strip_top / 144.0f < ms) ms = (float)strip_top / 144.0f;
    const int mw = (int)(256.0f * ms + 0.5f), mh = (int)(144.0f * ms + 0.5f);
    *film = SDL_Rect{(win_w - mw) / 2, (strip_top - mh) / 2, mw, mh};
    lb_th_strip = SDL_Rect{0, strip_top, win_w, strip};
    lb_th_field = *field;
    lb_th_win_w = win_w;
    return true;
}

static SDL_Texture* lb_th_tex = nullptr;
static SDL_Renderer* lb_th_renderer = nullptr;
static int lb_th_w = 0, lb_th_h = 0;
static unsigned lb_th_gen = ~0u;

static void lb_theatre_draw(FrontendPresentation& presentation, const SDL_Rect& film) {
    SDL_Renderer* r = presentation.renderers[1];
    if (!r) return;
    static std::vector<uint32_t> buf(512u * 288u);
    int w = 0, h = 0;
    if (lb_th_renderer != r) {   /* a new renderer: start over */
        lb_th_tex = nullptr;
        lb_th_renderer = r;
        lb_th_gen = ~0u;
    }
    if (lb_visuals_theatre_frame(buf.data(), (int)buf.size(), &w, &h, &lb_th_gen)) {
        if (w <= 0 || h <= 0) {
            if (lb_th_tex) SDL_DestroyTexture(lb_th_tex);
            lb_th_tex = nullptr;
            lb_th_w = lb_th_h = 0;
        } else {
            if (!lb_th_tex || lb_th_w != w || lb_th_h != h) {
                if (lb_th_tex) SDL_DestroyTexture(lb_th_tex);
                lb_th_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                lb_th_w = w;
                lb_th_h = h;
                if (lb_th_tex) {
                    SDL_SetTextureBlendMode(lb_th_tex, SDL_BLENDMODE_NONE);
                    SDL_SetTextureScaleMode(lb_th_tex, SDL_ScaleModeLinear);
                }
            }
            if (lb_th_tex) SDL_UpdateTexture(lb_th_tex, nullptr, buf.data(), w * (int)sizeof(uint32_t));
        }
    }
    /* A cinema: everything above the strip is black but the film, and the
       strip's own black frames the buttons (the bottom screen drawn under
       it is black there too, so only the band shows). */
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    const SDL_Rect above{0, 0, lb_th_win_w, lb_th_strip.y};
    SDL_RenderFillRect(r, &above);
    const SDL_Rect left{0, lb_th_strip.y, lb_th_field.x, lb_th_strip.h};
    const SDL_Rect right{lb_th_field.x + lb_th_field.w, lb_th_strip.y,
                         lb_th_win_w - (lb_th_field.x + lb_th_field.w), lb_th_strip.h};
    if (left.w > 0) SDL_RenderFillRect(r, &left);
    if (right.w > 0) SDL_RenderFillRect(r, &right);
    if (lb_th_tex) SDL_RenderCopy(r, lb_th_tex, nullptr, &film);
}
