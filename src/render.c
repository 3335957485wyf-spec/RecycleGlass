#include "render.h"
#include "i18n.h"
#include "resource.h"
#include "glass.h"

/* ------------------------------------------------------------- constants */

typedef struct {
    int pad, card_w, card_h, radius;
    int title_fs, sub_fs, icon, icon_left;
    int pill_h, pill_gap, pill_r, pill_max_w;
    int shadow_blur, shadow_dy;
} BaseMetrics;

static const BaseMetrics BASE = {
    34, 302, 118, 32,
    22, 16, 64, 18,
    32, 8, 10, 96,
    12, 6
};

/* Layout, rebuilt by render_set_scale(). */
static float g_scale = 1.0f;
static int   PAD, CARD_W, CARD_H;
static int   TITLE_FS, SUB_FS, ICON, ICON_X, ICON_Y;
static int   PILL_H, PILL_GAP, PILL_R, PILL_MAX_W;
static int   SHADOW_BLUR, SHADOW_DY, SHADOW_PAD;
static int   WINDOW_W, WINDOW_H;
/* Extra screen pixels captured around the card.  Refraction bends the actual
 * surroundings of the glass, so the backdrop crop has to reach past the card
 * edge; sampling only inside the card can merely smear the interior. */
static int   REFR_PAD;

/* ---------------------------------------------------------------- palette */

typedef struct {
    int tint_r, tint_g, tint_b;
    int text_r, text_g, text_b;
    int dim_r, dim_g, dim_b;
    ARGB border;
    ARGB pill_bg, pill_text;
    ARGB chip_bg, chip_border;
    ARGB pill_hover_bg, pill_hover_border;
    ARGB drag_wash, drag_ring;
    int rim_top_a, rim_bot_a;
    float tint_floor, tint_span;
    float opacity_floor, opacity_span;
    int shadow_alpha;
    int sheen_top, sheen_bottom, bottom_shade;
    int accent_r, accent_g, accent_b;
} Palette;

static ARGB ARGB_OF(int a, int r, int g, int b)
{
    return ((ARGB)(a & 0xFF) << 24) | ((ARGB)(r & 0xFF) << 16) |
           ((ARGB)(g & 0xFF) << 8) | (ARGB)(b & 0xFF);
}

/* Linear ARGB blend, used to ease colours between two states. */
static ARGB blend_argb(ARGB a, ARGB b, float t)
{
    int aa = (int)rg_lerp((float)((a >> 24) & 0xFF), (float)((b >> 24) & 0xFF), t);
    int ar = (int)rg_lerp((float)((a >> 16) & 0xFF), (float)((b >> 16) & 0xFF), t);
    int ag = (int)rg_lerp((float)((a >> 8) & 0xFF), (float)((b >> 8) & 0xFF), t);
    int ab = (int)rg_lerp((float)(a & 0xFF), (float)(b & 0xFF), t);
    return ARGB_OF(aa, ar, ag, ab);
}

/* A reusable diagonal accent gradient.  Gamma correction keeps the midpoint
 * from going muddy between high-chroma pairs such as peach→rose. */
static GpLineGradient *accent_gradient(float x, float y, float w, float h,
                                       ARGB from, ARGB to)
{
    GpRectF box;
    GpLineGradient *brush = NULL;

    box.X = x;
    box.Y = y;
    box.Width = w > 0.5f ? w : 1.0f;
    box.Height = h > 0.5f ? h : 1.0f;
    if (GdipCreateLineBrushFromRect(&box, from, to,
                                    LinearGradientModeForwardDiagonal,
                                    WrapModeTileFlipXY, &brush) == Ok)
        GdipSetLineGammaCorrection(brush, TRUE);
    return brush;
}

static int style_index(int style)
{
    return rg_clampi(style, 0, 2);
}

#define ACCENT_COUNT 6

static int accent_index(int accent)
{
    return rg_clampi(accent, 0, ACCENT_COUNT - 1);
}

typedef struct {
    BYTE from[3], to[3], text[3];
} AccentDef;

/* Each preset is a two-stop material colour, not one flat swatch.  The text
 * stop is deeper on light glass so labels remain legible; on dark glass it is
 * lifted toward the light end instead. */
static const AccentDef ACCENTS_LIGHT[ACCENT_COUNT] = {
    { { 100, 202, 255 }, {  66, 106, 255 }, {   0,  98, 220 } }, /* glacier */
    { { 205, 157, 255 }, { 112,  88, 255 }, { 104,  66, 205 } }, /* violet  */
    { { 126, 241, 201 }, {  27, 160, 133 }, {   0, 124, 103 } }, /* mint    */
    { { 255, 178, 154 }, { 241,  91, 137 }, { 189,  60,  86 } }, /* peach   */
    { { 255, 212, 116 }, { 238, 146,  55 }, { 158,  86,   0 } }, /* amber   */
    { { 214, 244, 128 }, { 109, 175,  58 }, {  79, 120,  14 } }  /* lime    */
};

static const AccentDef ACCENTS_DARK[ACCENT_COUNT] = {
    { { 136, 214, 255 }, { 133, 157, 255 }, { 160, 208, 255 } },
    { { 222, 184, 255 }, { 169, 142, 255 }, { 211, 178, 255 } },
    { { 160, 250, 218 }, { 100, 216, 184 }, { 143, 233, 203 } },
    { { 255, 198, 174 }, { 255, 146, 174 }, { 255, 190, 176 } },
    { { 255, 225, 146 }, { 250, 183,  99 }, { 251, 208, 144 } },
    { { 230, 250, 163 }, { 166, 220, 110 }, { 212, 236, 164 } }
};

static const AccentDef *accent_def(int dark, int accent)
{
    return dark ? &ACCENTS_DARK[accent_index(accent)]
                : &ACCENTS_LIGHT[accent_index(accent)];
}

static ARGB accent_argb(int dark, int accent, int alpha)
{
    const AccentDef *a = accent_def(dark, accent);
    return ARGB_OF(alpha, a->text[0], a->text[1], a->text[2]);
}

static ARGB accent_from(int dark, int accent, int alpha)
{
    const AccentDef *a = accent_def(dark, accent);
    return ARGB_OF(alpha, a->from[0], a->from[1], a->from[2]);
}

static ARGB accent_to(int dark, int accent, int alpha)
{
    const AccentDef *a = accent_def(dark, accent);
    return ARGB_OF(alpha, a->to[0], a->to[1], a->to[2]);
}

void render_accent_palette(int dark, int accent,
                           ARGB *from, ARGB *to, ARGB *text)
{
    if (from) *from = accent_from(dark, accent, 255);
    if (to)   *to = accent_to(dark, accent, 255);
    if (text) *text = accent_argb(dark, accent, 255);
}

static const Palette PAL_LIGHT = {
    /* The material has to sit BELOW the page, not on it: a near-white tint on
     * white paper is a no-op, and the light theme ended up with no panel at
     * all (measured: body 244 against a 250 page, six levels of separation).
     * 228-236 reads as a sheet of frosted glass on paper. */
    238, 240, 246,
    16, 17, 21,
    48, 50, 60,
    /*border*/         0x14000000,
    /*pill_bg*/        0xEEFFFFFF,
    /*pill_text*/      0xFF1A1B1F,
    /*chip_bg*/        0x0C000000,
    /*chip_border*/    0x10000000,
    /*pill_hover_bg*/  0x0C000000,
    /*pill_hover_border*/ 0xC8007AFF,
    /*drag_wash*/      0x22007AFF,
    /*drag_ring*/      0xDC007AFF,
    122, 40,
    /* `fac` IS the opacity slider value: 0 leaves only the content floating,
     * 1 is the full frosted body.  The old floor let the slider stop at 68%
     * no matter how far it was pulled, which is the "不能调成透明" report. */
    /* Material 0 IS clear glass: no tint at all, so what is behind the card
     * shows through untouched.  The span carries the whole range. */
    0.00f, 0.98f,
    0.0f, 1.0f,
    48,
    118, 6, 10,
    0, 122, 255
};

static const Palette PAL_DARK = {
    34, 36, 46,
    246, 247, 250,
    198, 202, 214,
    /*border*/         0x00FFFFFF,
    /*pill_bg*/        0x1AFFFFFF,
    /*pill_text*/      0xFFF6F7FA,
    /*chip_bg*/        0x10FFFFFF,
    /*chip_border*/    0x14FFFFFF,
    /*pill_hover_bg*/  0x34FFFFFF,
    /*pill_hover_border*/ 0x82FFFFFF,
    /*drag_wash*/      0x287EBAFC,
    /*drag_ring*/      0xD77EBAFC,
    122, 34,
    /* Same mapping as the light palette: the slider is the body alpha. */
    0.00f, 0.98f,
    0.0f, 1.0f,
    130,
    44, 0, 26,
    126, 186, 252
};

/* --------------------------------------------------------------- GDI+ glue */

static ULONG_PTR g_gdiplus_token = 0;
static HINSTANCE  g_inst = NULL;
static GpFont    *g_title_font = NULL;
static GpFont    *g_sub_font = NULL;
/* Per-font ink-centre compensation, measured once in build_fonts(). */
static float      g_ink_title = 0.0f;
static float      g_ink_sub = 0.0f;
static GpFontFamily *g_family = NULL;
static GpStringFormat *g_fmt = NULL;
static GpImage   *g_icon = NULL;
#define ICON_VARIANTS (ACCENT_COUNT * 2)
static GpImage   *g_icon_variant[ICON_VARIANTS];
static IStream   *g_icon_stream = NULL;

/* Size the icon is baked down to: enough for a 2x-DPI card, small enough that
 * drawing it is a plain bitmap blit. */
#define ICON_BAKE 256

/* Two coverage masks are enough: the card and the context-menu panel.  Keeping
 * both resident stops them evicting each other while the menu is open. */
typedef struct {
    BYTE     *px;
    int       w, h, r;
    ULONGLONG used;
} MaskSlot;

static MaskSlot g_masks[2];
static ULONGLONG g_mask_clock;

static GpBitmap *g_shadow = NULL;
static int       g_shadow_w = 0, g_shadow_h = 0, g_shadow_r = 0, g_shadow_a = -1;

static GpBitmap *g_rim = NULL;
static int g_rim_w, g_rim_h, g_rim_r, g_rim_a, g_rim_brd;
static int g_rim_style;
static int g_rim_light;
static ARGB g_rim_accent, g_rim_accent_b;
static float g_rim_env;

static BYTE *g_body = NULL;
static size_t g_body_cap = 0;
static unsigned g_body_gen;        /* bumped whenever g_body is reallocated */
static void   *g_body_bmp;         /* cached GDI+ wrapper around g_body */
static unsigned g_body_bmp_gen;
static int     g_body_w, g_body_h;

/* Cached content layer (icon + title).  The glass body is backdrop-dependent
 * and is repainted every frame; keeping the GDI+ text and icon in their own
 * layer is what lets a drag rebuild the card inside one display frame. */
static BYTE *g_static = NULL;
static int   g_static_w = 0, g_static_h = 0;

/* Bumped whenever a cache is dropped or the layout scale changes, so callers
 * that skip identical frames know a repaint is required anyway. */
static unsigned g_render_epoch;

unsigned render_epoch(void)
{
    return g_render_epoch;
}

typedef struct {
    int w, h, dark, accent, icon_style;
    unsigned epoch;
} ContentKey;

static ContentKey g_skey;
static BOOL       g_skey_valid;

RECT g_hit_open  = { 0, 0, 0, 0 };
RECT g_hit_empty = { 0, 0, 0, 0 };
RECT g_hit_menu  = { 0, 0, 0, 0 };

#define SQ_N     RG_SQUIRCLE_N
#define SQ_STEPS 144    /* points per corner quarter */

/* --------------------------------------------------------------- geometry */

static int scale_k(int base, int lo)
{
    int v = (int)lround((double)base * (double)g_scale);
    return v < lo ? lo : v;
}

void render_set_scale(float s)
{
    s = rg_clampf(s, 0.6f, 2.5f);
    g_scale = s;
    g_render_epoch++;
    glass_set_scale(s);

    PAD        = scale_k(BASE.pad, 16);
    CARD_W     = scale_k(BASE.card_w, 200);
    CARD_H     = scale_k(BASE.card_h, 78);
    TITLE_FS   = scale_k(BASE.title_fs, 11);
    SUB_FS     = scale_k(BASE.sub_fs, 9);
    ICON       = scale_k(BASE.icon, 34);
    /* compute_layout() re-derives these from the LIVE corner radius so the
     * icon inset, its gap to the text and the pill gap all track the same
     * golden scale the menu uses.  These are just sane pre-first-paint values. */
    ICON_X     = PAD + scale_k(BASE.icon_left, 10);
    ICON_Y     = PAD + (CARD_H - ICON) / 2;
    PILL_H     = scale_k(BASE.pill_h, 18);
    /* A pill is a capsule: its radius is half its height, the innermost
     * concentric step of the whole layout.  Its gap is R / phi^3. */
    PILL_GAP   = scale_k(rg_phi_step(rg_phi_unit((float)BASE.radius), 2), 4);
    PILL_R     = PILL_H / 2;
    PILL_MAX_W = scale_k(BASE.pill_max_w, 52);
    SHADOW_BLUR = scale_k(BASE.shadow_blur, 4);
    SHADOW_DY   = scale_k(BASE.shadow_dy, 2);
    SHADOW_PAD  = SHADOW_BLUR * 2 + 4;
    REFR_PAD    = scale_k(40, 24);
    WINDOW_W    = PAD * 2 + CARD_W;
    WINDOW_H    = PAD * 2 + CARD_H;

    render_drop_caches();
}

float render_scale(void) { return g_scale; }
int   render_win_w(void) { return WINDOW_W; }
int   render_win_h(void) { return WINDOW_H; }
int   render_card_w(void) { return CARD_W; }
int   render_card_h(void) { return CARD_H; }
int   render_pad(void) { return PAD; }
int   render_shadow_pad(void) { return SHADOW_PAD; }
int   render_refract_pad(void) { return REFR_PAD; }

/* ------------------------------------------------------- squircle geometry */

/* One axis of the Lamé curve: sign(c) * r * |c|^(2/n). */
static float sq_axis(float c, float r)
{
    float s = (c >= 0.0f) ? 1.0f : -1.0f;
    float a = fabsf(c);
    if (a < 1e-6f)
        return 0.0f;
    return s * r * powf(a, 2.0f / SQ_N);
}

/* Emits the outline of a rounded box whose corners are superellipse quarters. */
static int squircle_points(float x, float y, float w, float h, float radius,
                           GpPointF *pts, int cap)
{
    static const float start[4] = { 180.0f, 270.0f, 0.0f, 90.0f };
    float cx[4], cy[4];
    float r = radius;
    int n = 0, c, i;

    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;
    if (r < 0.0f) r = 0.0f;

    cx[0] = x + r;         cy[0] = y + r;          /* TL */
    cx[1] = x + w - r;     cy[1] = y + r;          /* TR */
    cx[2] = x + w - r;     cy[2] = y + h - r;      /* BR */
    cx[3] = x + r;         cy[3] = y + h - r;      /* BL */

    for (c = 0; c < 4; c++)
    {
        for (i = 0; i <= SQ_STEPS; i++)
        {
            float deg = start[c] + 90.0f * (float)i / (float)SQ_STEPS;
            float t = deg * 3.14159265358979f / 180.0f;
            float px = cx[c] + sq_axis(cosf(t), r);
            float py = cy[c] + sq_axis(sinf(t), r);

            if (n > 0 && fabsf(px - pts[n - 1].X) < 0.01f &&
                fabsf(py - pts[n - 1].Y) < 0.01f)
                continue;
            if (n >= cap)
                return n;
            pts[n].X = px;
            pts[n].Y = py;
            n++;
        }
    }
    return n;
}

static GpPath *make_squircle(float x, float y, float w, float h, float radius)
{
    GpPointF pts[4 * (SQ_STEPS + 1) + 4];
    GpPath *path = NULL;
    int n;

    n = squircle_points(x, y, w, h, radius, pts, RG_COUNT(pts));
    if (n < 3)
        return NULL;

    if (GdipCreatePath(FillModeAlternate, &path) != Ok)
        return NULL;

    /* One call for the whole outline: 4*97 points, which at a 1 px pen keeps
     * every facet below a pixel.  The old 20-segments-per-corner polyline was
     * coarse enough to show as steps along the flat part of a corner. */
    GdipStartPathFigure(path);
    GdipAddPathLine2(path, pts, n);
    GdipClosePathFigure(path);

    return path;
}

/* ------------------------------------------------------------- soft masks */

static void blur_alpha(BYTE *a, int w, int h, int r)
{
    BYTE *tmp;
    int x, y, pass, div;

    if (r < 1)
        return;

    tmp = (BYTE *)malloc((size_t)w * h);
    if (!tmp)
        return;

    div = r * 2 + 1;

    for (pass = 0; pass < 3; pass++)
    {
        /* horizontal */
        for (y = 0; y < h; y++)
        {
            BYTE *srow = a + (size_t)y * w;
            BYTE *drow = tmp + (size_t)y * w;
            int sum = 0;

            for (x = -r; x <= r; x++)
                sum += srow[rg_clampi(x, 0, w - 1)];

            for (x = 0; x < w; x++)
            {
                drow[x] = (BYTE)(sum / div);
                sum += srow[rg_clampi(x + r + 1, 0, w - 1)] -
                       srow[rg_clampi(x - r, 0, w - 1)];
            }
        }
        /* vertical */
        for (x = 0; x < w; x++)
        {
            int sum = 0;
            for (y = -r; y <= r; y++)
                sum += tmp[(size_t)rg_clampi(y, 0, h - 1) * w + x];

            for (y = 0; y < h; y++)
            {
                a[(size_t)y * w + x] = (BYTE)(sum / div);
                sum += tmp[(size_t)rg_clampi(y + r + 1, 0, h - 1) * w + x] -
                       tmp[(size_t)rg_clampi(y - r, 0, h - 1) * w + x];
            }
        }
    }

    free(tmp);
}

/* Anti-aliased coverage of a superellipse box (0..255), cached per size.
 *
 * The outline is a superellipse, so a row's horizontal boundary has a closed
 * form: at distance d from the top edge the corner arc sits at
 *     x = r - r * (1 - ((r-d)/r)^n)^(1/n)
 * and every other row is a straight run.  Computing that per row and ramping
 * across the boundary pixel gives exact coverage for a fraction of the cost of
 * a supersampled GDI+ fill - and, unlike a 4x4 average, it cannot quantise to
 * 1/16 alpha steps.  Those steps were the sawtooth along the shallow parts of
 * the corner. */
static void mask_build(BYTE *dst, int w, int h, int radius)
{
    /* Exact coverage, but the 16 sub-rows are sampled per *row*, not per
     * pixel: the powf() pair only runs 16 times per scanline and the per-pixel
     * work is a couple of compares, with per-sub-row integration reserved for
     * the narrow corner bands.  The naive per-pixel version cost ~40 ms on the
     * first menu frame - the entire "menu opens with a hitch" report. */
    const int S = 16;
    const float inv_n = 1.0f / SQ_N;
    float r = (float)radius;
    float hy, straight_y;
    float ls[16], rs[16];
    int outside[16];
    int y, x, sy;

    if (r * 2.0f > (float)w) r = (float)w * 0.5f;
    if (r * 2.0f > (float)h) r = (float)h * 0.5f;
    if (r < 0.0f) r = 0.0f;

    hy = (float)h * 0.5f;
    straight_y = hy - r;

    if (r <= 0.0f)
    {
        memset(dst, 255, (size_t)w * h);
        return;
    }

    for (y = 0; y < h; y++)
    {
        BYTE *row = dst + (size_t)y * w;
        float lmin = 1e9f, lmax = -1e9f, rmin = 1e9f, rmax = -1e9f;
        int any_inside = 0;

        /* Only the straight middle band is fully covered.  The corner bands
         * above and below it MUST go through the arc integration: filling
         * them with 255 paints a square cap across the top and bottom of the
         * card, which is exactly the "right-angle ring" around the glass.
         * (The old test used the corner bands as the fast path - inverted.) */
        if ((float)y >= r && (float)(y + 1) <= (float)h - r)
        {
            memset(row, 255, (size_t)w);
            continue;
        }

        for (sy = 0; sy < S; sy++)
        {
            float fy = (float)y + ((float)sy + 0.5f) / (float)S;
            float dy = fabsf(fy - hy) - straight_y;
            float dx;

            if (dy < 0.0f) dy = 0.0f;
            if (dy >= r)
            {
                outside[sy] = 1;
                continue;
            }
            outside[sy] = 0;
            dx = r * powf(1.0f - powf(dy / r, SQ_N), inv_n);
            ls[sy] = r - dx;
            rs[sy] = (float)w - ls[sy];
            any_inside = 1;
            if (ls[sy] < lmin) lmin = ls[sy];
            if (ls[sy] > lmax) lmax = ls[sy];
            if (rs[sy] < rmin) rmin = rs[sy];
            if (rs[sy] > rmax) rmax = rs[sy];
        }

        if (!any_inside)
        {
            /* Rows fully outside the shape must be written as empty - the
             * buffer is malloc'd, and leaving it at garbage made the corners
             * render as semi-transparent squares (the "right-angle ring"). */
            memset(row, 0, (size_t)w);
            continue;
        }

        for (x = 0; x < w; x++)
        {
            float fx = (float)x, fx1 = (float)(x + 1);
            float cov;
            int s;

            if (fx1 <= lmin)        { row[x] = 0;    continue; }
            if (fx >= lmax && fx1 <= rmin) { row[x] = 255; continue; }
            if (fx >= rmax)         { row[x] = 0;    continue; }

            cov = 0.0f;
            for (s = 0; s < S; s++)
            {
                float lo, hi, c;
                if (outside[s])
                    continue;
                lo = fx > ls[s] ? fx : ls[s];
                hi = fx1 < rs[s] ? fx1 : rs[s];
                c = hi - lo;
                if (c > 0.0f)
                    cov += c > 1.0f ? 1.0f : c;
            }
            row[x] = (BYTE)(cov * (255.0f / (float)S) + 0.5f);
        }
    }

}

static const BYTE *card_mask(int w, int h, int radius)
{
    MaskSlot *slot = NULL;
    int i;

    for (i = 0; i < 2; i++)
    {
        if (g_masks[i].px && g_masks[i].w == w && g_masks[i].h == h &&
            g_masks[i].r == radius)
        {
            g_masks[i].used = ++g_mask_clock;
            return g_masks[i].px;
        }
    }

    /* Pick the least recently used slot. */
    slot = &g_masks[0];
    for (i = 1; i < 2; i++)
        if (g_masks[i].used < slot->used || !slot->px)
            slot = &g_masks[i];

    free(slot->px);
    slot->px = (BYTE *)malloc((size_t)w * h);
    slot->w = w;
    slot->h = h;
    slot->r = radius;
    slot->used = ++g_mask_clock;
    if (!slot->px)
        return NULL;

    mask_build(slot->px, w, h, radius);
    return slot->px;
}

/* Card-edge distance field, defined with the rim further down. */
static float card_sdf(float px, float py, float w, float h, float r);

/* Soft drop shadow: black, alpha from a blurred card silhouette. */
static GpBitmap *shadow_sprite(int cw, int ch, int radius, int alpha)
{
    int w = cw + SHADOW_PAD * 2;
    int h = ch + SHADOW_PAD * 2;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GpPath *path = NULL;
    GpSolidFill *brush = NULL;
    BitmapData bd;
    BYTE *mask;
    int x, y;

    if (g_shadow && g_shadow_w == w && g_shadow_h == h &&
        g_shadow_r == radius && g_shadow_a == alpha)
        return g_shadow;

    if (g_shadow)
    {
        GdipDisposeImage(g_shadow);
        g_shadow = NULL;
    }
    g_shadow_w = w;
    g_shadow_h = h;
    g_shadow_r = radius;
    g_shadow_a = alpha;

    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB, NULL, &bmp) != Ok)
        return NULL;

    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipGraphicsClear(g, 0);

    path = make_squircle((float)SHADOW_PAD, (float)SHADOW_PAD,
                         (float)(cw - 1), (float)(ch - 1), (float)radius);
    if (path)
    {
        GdipCreateSolidFill(0xFFFFFFFF, &brush);
        GdipFillPath(g, (GpBrush *)brush, path);
        GdipDeleteBrush((GpBrush *)brush);
        GdipDeletePath(path);
    }
    GdipDeleteGraphics(g);

    mask = (BYTE *)calloc((size_t)w * h, 1);
    if (!mask)
    {
        GdipDisposeImage(bmp);
        return NULL;
    }

    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(bmp, NULL, ImageLockModeRead | ImageLockModeWrite,
                           PixelFormat32bppPARGB, &bd) == Ok)
    {
        for (y = 0; y < h; y++)
        {
            const BYTE *src = (const BYTE *)bd.Scan0 + (size_t)y * bd.Stride;
            for (x = 0; x < w; x++)
                mask[(size_t)y * w + x] = src[x * 4 + 3];
        }

        blur_alpha(mask, w, h, SHADOW_BLUR);

        for (y = 0; y < h; y++)
        {
            BYTE *dst = (BYTE *)bd.Scan0 + (size_t)y * bd.Stride;
            for (x = 0; x < w; x++)
            {
                int a = (int)mask[(size_t)y * w + x] * alpha / 255;
                /* Nothing of the drop is kept under the card.  The body holds
                 * ~9% transparency so the capture can be un-composited, and a
                 * near-black backing under it turns clear glass into a grey
                 * veil - the drop belongs outside the silhouette only. */
                {
                    float dc = card_sdf((float)x - (float)SHADOW_PAD + 0.5f,
                                        (float)y - (float)SHADOW_PAD +
                                            (float)SHADOW_DY + 0.5f,
                                        (float)cw, (float)ch, (float)radius);
                    if (dc < 0.5f)
                        a = 0;
                }
                /* A large soft shadow spans ~40 px with fewer than 90 alpha
                 * levels, so the 8-bit write quantises it into visible contour
                 * bands - the "not smooth" outer ring.  A 4x4 ordered dither of
                 * at most a level or two breaks the steps up without adding
                 * brightness. */
                {
                    static const signed char bayer[16] = {
                        -8, 0, -6, 2, 4, -4, 6, -2,
                        -5, 3, -7, 1, 7, -1, 5, -3
                    };
                    a += bayer[((y & 3) << 2) | (x & 3)] * a / 900;
                }
                /* premultiplied black */
                dst[x * 4 + 0] = 0;
                dst[x * 4 + 1] = 0;
                dst[x * 4 + 2] = 0;
                dst[x * 4 + 3] = (BYTE)rg_clampi(a, 0, 255);
            }
        }
        GdipBitmapUnlockBits(bmp, &bd);
    }

    free(mask);
    g_shadow = bmp;
    return bmp;
}

/* ------------------------------------------------------------------- fonts */

/* Cached line heights: measured once per font rebuild so the layout maths in
 * render_card() never needs a Graphics object. */
static float g_title_h = 20.0f;
static float g_sub_h = 14.0f;

static void measure_text(GpGraphics *g, const wchar_t *s, GpFont *font,
                         float *out_w, float *out_h);
static void measure_font_heights(void);

static void drop_fonts(void)
{
    if (g_title_font) { GdipDeleteFont(g_title_font); g_title_font = NULL; }
    if (g_sub_font)   { GdipDeleteFont(g_sub_font);   g_sub_font = NULL; }
    /* g_family is deliberately kept: it is shared with the menu layer for the
     * whole process lifetime, so derived GpFont objects can never dangle. */
}

/* MiSans - Xiaomi's official typeface, free for commercial use.  Only the
 * glyphs the UI can actually draw are embedded: tools/make_font_subset.py
 * unions every L"..." literal in src/ with the full printable ASCII range
 * (236 chars, 62 KB per weight) and subsets the two TTFs into res/, which
 * ship as RCDATA inside the exe.  They load through a private GDI+
 * memory-font collection - nothing to install, nothing to carry alongside
 * the exe.  A glyph outside the subset would silently fall back to the
 * system chain below; the char-set policy makes that unreachable for every
 * string the app draws. */
static GpFontCollection *g_misans_coll = NULL;
static BOOL              g_misans_tried = FALSE;

static GpFontFamily *load_misans(void)
{
    static const int ids[2] = { IDR_MISANS_REG, IDR_MISANS_BOLD };
    GpFontFamily *fam = NULL;
    int i;

    if (!g_misans_tried)
    {
        g_misans_tried = TRUE;
        if (GdipNewPrivateFontCollection(&g_misans_coll) == Ok)
        {
            for (i = 0; i < 2; i++)
            {
                HRSRC  rs   = FindResourceW(NULL, MAKEINTRESOURCEW(ids[i]),
                                            (LPCWSTR)RT_RCDATA);
                HGLOBAL res = rs ? LoadResource(NULL, rs) : NULL;
                void  *data = res ? LockResource(res) : NULL;
                DWORD size = rs ? SizeofResource(NULL, rs) : 0;

                /* the resource stays mapped for the process lifetime, which
                 * is exactly what GdipPrivateAddMemoryFont requires */
                if (data && size > 0)
                    GdipPrivateAddMemoryFont(g_misans_coll, data, size);
            }
            GdipCreateFontFamilyFromName(L"MiSans", g_misans_coll, &fam);
        }
        if (!fam && g_misans_coll)
        {
            GdipDeletePrivateFontCollection(&g_misans_coll);
            g_misans_coll = NULL;
        }
    }
    else if (g_misans_coll)
    {
        GdipCreateFontFamilyFromName(L"MiSans", g_misans_coll, &fam);
    }
    return fam;
}

/* The ONE family every layer of the UI draws with.  Priority: bundled MiSans,
 * an installed MiSans, then the platform defaults. */
GpFontFamily *render_app_family(void)
{
    static const wchar_t *cjk_names[] = { L"MiSans", L"Microsoft YaHei UI",
                                          L"Microsoft YaHei", L"SimHei" };
    static const wchar_t *lat_names[] = { L"MiSans", L"Segoe UI", L"Tahoma" };
    const wchar_t **names;
    int count, i;

    if (g_family)
        return g_family;

    {
        GpFontFamily *fam = load_misans();
        if (fam)
        {
            g_family = fam;
            return g_family;
        }
    }

    names = i18n_is_cjk() ? cjk_names : lat_names;
    count = i18n_is_cjk() ? RG_COUNT(cjk_names) : RG_COUNT(lat_names);
    for (i = 0; i < count && !g_family; i++)
    {
        GpFontFamily *fam = NULL;
        if (GdipCreateFontFamilyFromName(names[i], NULL, &fam) == Ok && fam)
            g_family = fam;
    }
    if (!g_family)
        GdipGetGenericFontFamilySansSerif(&g_family);
    return g_family;
}

static void build_fonts(void)
{
    drop_fonts();

    g_family = render_app_family();
    if (g_family)
    {
        GdipCreateFont(g_family, (REAL)TITLE_FS, FontStyleBold, UnitPixel, &g_title_font);
        GdipCreateFont(g_family, (REAL)SUB_FS, FontStyleRegular, UnitPixel, &g_sub_font);
    }

    measure_font_heights();

    g_ink_title = render_text_ink_offset(g_title_font);
    g_ink_sub   = render_text_ink_offset(g_sub_font);
}

static void measure_font_heights(void)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    float w = 0.0f, h = 0.0f;

    if (!g_gdiplus_token || !g_title_font || !g_sub_font)
        return;

    if (GdipCreateBitmapFromScan0(8, 8, 32, PixelFormat32bppPARGB, NULL, &bmp) != Ok)
        return;
    if (GdipGetImageGraphicsContext(bmp, &g) != Ok)
    {
        GdipDisposeImage(bmp);
        return;
    }

    measure_text(g, T(S_APP_NAME), g_title_font, &w, &h);
    if (h > 1.0f)
        g_title_h = h;

    measure_text(g, L"Ag", g_sub_font, &w, &h);
    if (h > 1.0f)
        g_sub_h = h;

    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);
}

static void measure_text(GpGraphics *g, const wchar_t *s, GpFont *font,
                         float *out_w, float *out_h)
{
    GpRectF layout = { 0.0f, 0.0f, 4000.0f, 4000.0f };
    GpRectF bound;
    INT cps = 0, lines = 0;

    if (out_w) *out_w = 0.0f;
    if (out_h) *out_h = 0.0f;
    if (!s || !*s || !font || !g)
        return;

    if (GdipMeasureString(g, s, -1, font, &layout, g_fmt, &bound, &cps, &lines) == Ok)
    {
        if (out_w) *out_w = bound.Width;
        if (out_h) *out_h = bound.Height;
    }
}

static void draw_text(GpGraphics *g, const wchar_t *s, GpFont *font,
                      float x, float y, float w, ARGB color)
{
    GpSolidFill *brush = NULL;
    GpRectF rect;
    float off = 0.0f;

    if (!s || !*s || !font)
        return;

    /* Shift the line box so the GLYPHS end up centred, not the font's line
     * box (see render_text_ink_offset). */
    if (font == g_title_font)
        off = g_ink_title;
    else if (font == g_sub_font)
        off = g_ink_sub;

    rect.X = x;
    rect.Y = y + off;
    rect.Width = w;
    rect.Height = 4000.0f;

    if (GdipCreateSolidFill(color, &brush) != Ok)
        return;

    GdipDrawString(g, s, -1, font, &rect, g_fmt, (GpBrush *)brush);
    GdipDeleteBrush((GpBrush *)brush);
}

/* Keep a single line of card text inside maxw, ending it with an ellipsis.
 * The subtitle is "338 项 · 404.41 MB" - a bin with thousands of items and a
 * multi-gigabyte size measures wider than the text column, and GDI+ would then
 * wrap it onto a second line that runs straight into the action pills. */
static void ellipsize_text(GpGraphics *g, wchar_t *s, GpFont *font, float maxw)
{
    float w = 0.0f;
    size_t n, max;

    if (!s || !*s || !font || maxw <= 1.0f)
        return;
    measure_text(g, s, font, &w, NULL);
    if (w <= maxw)
        return;

    max = wcslen(s);
    if (max > 120)
        max = 120;
    for (n = max; n > 0; n--)
    {
        s[n] = 0x2026;          /* … */
        s[n + 1] = 0;
        measure_text(g, s, font, &w, NULL);
        if (w <= maxw)
            return;
        s[n] = 0;
    }
    s[0] = 0x2026;
    s[1] = 0;
}

/* ------------------------------------------------- ink-centre compensation */

/* GDI+ "vertical centring" centres its OWN line box - ascent + descent plus
 * leading - and that box is not symmetric around the ideographic square:
 * MiSans reports ascent 1.056 em and descent 0.333 em, so the box centre sits
 * ~0.11 em BELOW the visual centre of a hanzi.  Every label therefore landed
 * 1.5-2 px too high inside its pill, row or card - the "字都偏上" report.
 *
 * The offset is a constant per font, so measure it once: draw one full-square
 * hanzi into a scratch bitmap through the very same GDI+ path, find the ink's
 * vertical centre and compare it with the box centre.  A positive result means
 * "move the text down by this much". */
float render_text_ink_offset(GpFont *font)
{
    enum { PX = 64 };
    BYTE *bits;
    GpBitmap *bm = NULL;
    GpGraphics *g = NULL;
    GpSolidFill *br = NULL;
    GpStringFormat *fmt = NULL;
    GpRectF rc = { 0.0f, 0.0f, (REAL)PX, (REAL)PX };
    int y, top = -1, bottom = -1;
    float off = 0.0f;

    if (!font || !g_gdiplus_token)
        return 0.0f;

    bits = (BYTE *)calloc((size_t)PX * PX, 4);
    if (!bits)
        return 0.0f;

    if (GdipCreateBitmapFromScan0(PX, PX, PX * 4, PixelFormat32bppPARGB,
                                  bits, &bm) != Ok || !bm)
    {
        free(bits);
        return 0.0f;
    }
    if (GdipGetImageGraphicsContext(bm, &g) != Ok ||
        GdipCreateSolidFill(0xFFFFFFFF, &br) != Ok ||
        GdipCreateStringFormat(0, 0, &fmt) != Ok)
    {
        if (g) GdipDeleteGraphics(g);
        if (br) GdipDeleteBrush((GpBrush *)br);
        if (fmt) GdipDeleteStringFormat(fmt);
        GdipDisposeImage(bm);
        free(bits);
        return 0.0f;
    }

    GdipGraphicsClear(g, 0x00000000);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetStringFormatAlign(fmt, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter);
    GdipSetStringFormatFlags(fmt, StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    GdipDrawString(g, L"\x56FD", -1, font, &rc, fmt, (GpBrush *)br);

    for (y = 0; y < PX; y++)
    {
        const BYTE *row = bits + (size_t)y * PX * 4;
        int x;
        for (x = 0; x < PX; x++)
        {
            if (row[x * 4 + 3] > 16)
            {
                if (top < 0)
                    top = y;
                bottom = y;
                break;
            }
        }
    }

    if (top >= 0 && bottom >= top)
        off = (float)PX * 0.5f -
              ((float)top + (float)(bottom + 1)) * 0.5f;

    GdipDeleteStringFormat(fmt);
    GdipDeleteBrush((GpBrush *)br);
    GdipDeleteGraphics(g);
    GdipDisposeImage(bm);
    free(bits);
    return off;
}

/* -------------------------------------------------------------------- icon */

/* Turns a freshly loaded PNG into a small, fully decoded bitmap.
 *
 * GDI+ decodes PNG images lazily: drawing a 1920x1920 PNG at 18 px re-decodes
 * (and rescales) the whole thing on every single DrawImage call.  That alone
 * cost more than the entire card render - three times the frame time - so the
 * icon is baked down to a thumbnail once, at startup. */
static GpImage *bake_icon(GpImage *raw)
{
    GpImage *thumb = NULL;

    if (!raw)
        return NULL;

    if (GdipGetImageThumbnail(raw, ICON_BAKE, ICON_BAKE, (GpImage **)&thumb,
                              NULL, NULL) != Ok || !thumb)
    {
        GdipDisposeImage(raw);
        return NULL;
    }

    GdipDisposeImage(raw);
    return thumb;
}

/* Re-tints the blue chip baked into the icon while leaving the neutral white
 * glyph alone.  Four variants are prepared once at startup; drawing still
 * stays a plain bitmap blit. */
static BYTE *g_icon_pixels[ICON_VARIANTS];

static GpImage *accent_icon(GpImage *base, int accent, int dark)
{
    GpBitmap *variant = NULL;
    BitmapData bd;
    BYTE *px;
    int stride = ICON_BAKE * 4;
    int x, y;

    if (!base)
        return NULL;

    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits((GpBitmap *)base, NULL, ImageLockModeRead,
                           PixelFormat32bppPARGB, &bd) != Ok)
        return NULL;

    px = (BYTE *)calloc((size_t)stride * ICON_BAKE, 1);
    if (!px)
    {
        GdipBitmapUnlockBits((GpBitmap *)base, &bd);
        return NULL;
    }

    {
        ARGB c0 = accent_from(dark, accent, 255);
        ARGB c1 = accent_to(dark, accent, 255);
        int b0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, r0 = (c0 >> 16) & 0xFF;
        int b1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, r1 = (c1 >> 16) & 0xFF;

        for (y = 0; y < ICON_BAKE; y++)
        {
            const BYTE *src = (const BYTE *)bd.Scan0 + (size_t)y * bd.Stride;
            BYTE *row = px + (size_t)y * stride;
            for (x = 0; x < ICON_BAKE; x++)
            {
                const BYTE *q = src + x * 4;
                BYTE *d = row + x * 4;
                int hi = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2])
                                     : (q[1] > q[2] ? q[1] : q[2]);
                int lo = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2])
                                     : (q[1] < q[2] ? q[1] : q[2]);
                d[0] = q[0];
                d[1] = q[1];
                d[2] = q[2];
                d[3] = q[3];
                /* GDI+ scan0 is BGRA: q[0] is blue, q[2] is red.  The chip is
                 * remapped across the pair on a diagonal; the neutral glyph and
                 * its soft highlight are left untouched. */
                if (q[3] > 0 && hi - lo > 14 && q[0] >= q[2])
                {
                    float u = ((float)x * 0.42f + (float)y * 0.58f) /
                              (float)(ICON_BAKE - 1);
                    float shade = 0.86f + 0.10f * (float)hi / 255.0f;
                    int rr = (int)rg_lerp((float)r0, (float)r1, u) * shade;
                    int gg = (int)rg_lerp((float)g0, (float)g1, u) * shade;
                    int bb = (int)rg_lerp((float)b0, (float)b1, u) * shade;
                    int a = q[3];
                    d[0] = (BYTE)rg_clampi(bb * a / 255, 0, 255);
                    d[1] = (BYTE)rg_clampi(gg * a / 255, 0, 255);
                    d[2] = (BYTE)rg_clampi(rr * a / 255, 0, 255);
                }
            }
        }
    }
    GdipBitmapUnlockBits((GpBitmap *)base, &bd);

    if (GdipCreateBitmapFromScan0(ICON_BAKE, ICON_BAKE, stride,
                                  PixelFormat32bppPARGB, px, &variant) != Ok ||
        !variant)
    {
        free(px);
        return NULL;
    }

    g_icon_pixels[dark * ACCENT_COUNT + accent_index(accent)] = px;
    return (GpImage *)variant;
}

static void drop_icon_variants(void)
{
    int i;
    for (i = 0; i < ICON_VARIANTS; i++)
    {
        if (g_icon_variant[i])
        {
            GdipDisposeImage(g_icon_variant[i]);
            g_icon_variant[i] = NULL;
        }
        free(g_icon_pixels[i]);
        g_icon_pixels[i] = NULL;
    }
}

/* Tray and window icons are the same mark as the card, so the original icon
 * can follow the accent too instead of staying blue in the notification area.
 * GDI+ does the downscale because the stock 16 px frame of app-icon.ico is a
 * different drawing, not a resized one. */
HICON render_make_icon(int accent, int dark, int size)
{
    GpImage *src;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    HICON icon = NULL;
    BYTE *px;
    int stride;

    if (!g_icon || size <= 0)
        return NULL;

    src = g_icon_variant[(dark ? ACCENT_COUNT : 0) + accent_index(accent)];
    if (!src)
        src = g_icon;

    stride = size * 4;
    px = (BYTE *)calloc((size_t)stride * (size_t)size, 1);
    if (!px)
        return NULL;

    if (GdipCreateBitmapFromScan0(size, size, stride, PixelFormat32bppPARGB,
                                  px, &bmp) == Ok && bmp)
    {
        if (GdipGetImageGraphicsContext(bmp, &g) == Ok && g)
        {
            GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
            GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
            GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
            GdipGraphicsClear(g, 0x00000000);
            GdipDrawImageRectI(g, src, 0, 0, size, size);
            GdipDeleteGraphics(g);
        }
        if (GdipCreateHICONFromBitmap(bmp, &icon) != Ok)
            icon = NULL;
        GdipDisposeImage(bmp);
    }

    free(px);
    return icon;
}

static GpImage *load_png_resource(HINSTANCE inst, int id)
{
    HRSRC res;
    HGLOBAL glob;
    void *src;
    DWORD size;
    HGLOBAL mem;
    void *dst;
    IStream *stream = NULL;
    GpImage *img = NULL;

    res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res)
        return NULL;

    size = SizeofResource(inst, res);
    glob = LoadResource(inst, res);
    src = LockResource(glob);
    if (!src || size == 0)
        return NULL;

    mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem)
        return NULL;

    dst = GlobalLock(mem);
    if (!dst)
    {
        GlobalFree(mem);
        return NULL;
    }
    memcpy(dst, src, size);
    GlobalUnlock(mem);

    if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK)
    {
        if (GdipLoadImageFromStream(stream, &img) == Ok && img)
        {
            /* GDI+ decodes lazily: the stream (and its memory) must outlive
             * the image, otherwise every later DrawImage draws nothing. */
            g_icon_stream = stream;
            img = bake_icon(img);
        }
        else
        {
            stream->lpVtbl->Release(stream);
            img = NULL;
        }
    }

    if (!img)
        GlobalFree(mem);

    return img;
}

/* ------------------------------------------------------------------ public */

void render_init(HINSTANCE inst)
{
    GdiplusStartupInput in;

    g_inst = inst;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    GdiplusStartup(&g_gdiplus_token, &in, NULL);

    GdipStringFormatGetGenericTypographic(&g_fmt);
    if (g_fmt)
        GdipSetStringFormatFlags(g_fmt,
                                 StringFormatFlagsNoWrap |
                                 StringFormatFlagsNoClip);

    g_icon = load_png_resource(inst, IDR_APPICON_PNG);
    if (!g_icon)
    {
        /* No resource (e.g. a build without an .rc): fall back to the asset
         * next to the executable so the widget still shows its icon. */
        static const wchar_t *candidates[] = {
            L"res\\app-icon-1920.png",
            L"app-icon-1920.png"
        };
        int i;
        for (i = 0; i < RG_COUNT(candidates) && !g_icon; i++)
        {
            GpImage *raw = NULL;
            if (GetFileAttributesW(candidates[i]) != INVALID_FILE_ATTRIBUTES &&
                GdipLoadImageFromFile(candidates[i], &raw) == Ok)
                g_icon = bake_icon(raw);
        }
    }
    if (!g_icon)
    {
        /* Builds that embed only the Windows icon (rcedit / no windres) can
         * still feed the card from the shell icon at the largest size. */
        static const int ids[] = { IDR_APPICON, 1 };
        int i;
        for (i = 0; i < RG_COUNT(ids) && !g_icon; i++)
        {
            HICON hi = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(ids[i]),
                                         IMAGE_ICON, 256, 256,
                                         LR_DEFAULTCOLOR);
            if (hi)
            {
                GpBitmap *hb = NULL;
                if (GdipCreateBitmapFromHICON(hi, &hb) == Ok && hb)
                    g_icon = bake_icon((GpImage *)hb);
                DestroyIcon(hi);
            }
        }
    }
    if (g_icon)
    {
        int a;
        for (a = 0; a < ICON_VARIANTS; a++)
            g_icon_variant[a] = accent_icon(g_icon, a % ACCENT_COUNT,
                                            a / ACCENT_COUNT);
    }
    render_set_scale(1.0f);
}

void render_drop_caches(void)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        free(g_masks[i].px);
        g_masks[i].px = NULL;
        g_masks[i].w = g_masks[i].h = g_masks[i].r = 0;
        g_masks[i].used = 0;
    }
    g_mask_clock = 0;

    if (g_shadow)
    {
        GdipDisposeImage(g_shadow);
        g_shadow = NULL;
    }
    g_shadow_w = g_shadow_h = g_shadow_r = 0;
    g_shadow_a = -1;
    g_skey_valid = FALSE;
    g_render_epoch++;

    build_fonts();
}

void render_shutdown(void)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        free(g_masks[i].px);
        g_masks[i].px = NULL;
        g_masks[i].w = g_masks[i].h = g_masks[i].r = 0;
    }
    if (g_shadow)
    {
        GdipDisposeImage(g_shadow);
        g_shadow = NULL;
    }
    if (g_rim)
    {
        GdipDisposeImage(g_rim);
        g_rim = NULL;
    }
    if (g_body_bmp)
    {
        GdipDisposeImage((GpImage *)g_body_bmp);
        g_body_bmp = NULL;
    }
    free(g_body);
    g_body = NULL;
    g_body_cap = 0;
    g_body_gen++;
    free(g_static);
    g_static = NULL;
    g_static_w = g_static_h = 0;
    g_skey_valid = FALSE;
    drop_fonts();
    if (g_fmt)
    {
        GdipDeleteStringFormat(g_fmt);
        g_fmt = NULL;
    }
    drop_icon_variants();
    if (g_icon)
    {
        GdipDisposeImage(g_icon);
        g_icon = NULL;
    }
    if (g_icon_stream)
    {
        g_icon_stream->lpVtbl->Release(g_icon_stream);
        g_icon_stream = NULL;
    }
    if (g_gdiplus_token)
    {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0;
    }
}

BOOL render_surface_resize(RgSurface *s, int w, int h)
{
    BITMAPINFO bmi;
    HDC screen;

    if (!s)
        return FALSE;
    if (s->dc && s->w == w && s->h == h)
        return TRUE;

    render_surface_free(s);

    screen = GetDC(NULL);
    if (!screen)
        return FALSE;
    s->dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!s->dc)
        return FALSE;

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s->bmp = CreateDIBSection(s->dc, &bmi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    if (!s->bmp)
    {
        DeleteDC(s->dc);
        s->dc = NULL;
        return FALSE;
    }

    s->old = SelectObject(s->dc, s->bmp);
    s->w = w;
    s->h = h;
    return TRUE;
}

void render_surface_free(RgSurface *s)
{
    if (!s)
        return;
    if (s->gp)
    {
        GdipDisposeImage(s->gp);
        s->gp = NULL;
    }
    if (s->dc && s->old)
        SelectObject(s->dc, s->old);
    if (s->bmp)
        DeleteObject(s->bmp);
    if (s->dc)
        DeleteDC(s->dc);
    memset(s, 0, sizeof(*s));
}

/* Vertical sheen profile: bright at the top, fades by mid-card, dark at the
 * very bottom (a subtle "cast" edge). */
/* Smoothstep: zero slope at both ends, so two segments joined at a control
 * point meet without a crease. */
static float rg_smoothstep(float u)
{
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

/* Vertical light profile of the glass body.  The old version joined two
 * straight ramps at 55% height, and the slope change left a faint horizontal
 * crease across the card that read as "a line inside the blur" on smooth dark
 * wallpapers.  Both segments now ease in and out, so the curve is C1. */
static int sheen_alpha(int y, int h, int top_a, int bottom_a, int shade)
{
    float t = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
    float a;

    if (t < 0.55f)
        a = (float)top_a + ((float)bottom_a - (float)top_a) * rg_smoothstep(t / 0.55f);
    else
        a = (float)bottom_a * (1.0f - rg_smoothstep((t - 0.55f) / 0.45f));

    if (shade > 0 && t > 0.72f)
    {
        float u = (t - 0.72f) / 0.28f;
        a -= (float)shade * u * u;
    }

    return (int)lroundf(a);
}

/* ------------------------------------------------------------ layout maths */

typedef struct {
    int   cw, ch, ox, oy, radius;
    float text_x, content_w, title_w;
    float sub_gap, row_gap;
    float block_top, row_top;
    float sub_a, pill_a;
    int   dy, bw, total_w, bx0;
    int   menu_x, menu_y, menu_w, menu_h;
    float ht;
} Layout;

static int card_corner_radius(const RgRenderState *st)
{
    float reference = st->corner_radius > 0.0f ? st->corner_radius : (float)BASE.radius;
    int maximum = (CARD_W < CARD_H ? CARD_W : CARD_H) / 2;
    int r = (int)lroundf(rg_clampf(reference, 12.0f, 52.0f) * g_scale);
    return rg_clampi(r, 1, maximum > 1 ? maximum : 1);
}

static void compute_layout(const RgRenderState *st, Layout *L)
{
    float idle_h;
    /* One ruler for the whole app: U = R/phi is the inset of content from its
     * container edge, U1 = U/phi the gap between siblings, U2 = U1/phi the
     * innermost gap.  The menu derives PAD/U1/U2/U3 from exactly the same
     * number (common.h), so the icon's left, top and bottom margins, the gap
     * to the title, the right margin and the pill gap are no longer a pile of
     * unrelated 10/12/18/20 px constants. */
    float U, U1, U2;

    L->cw = CARD_W;
    L->ch = CARD_H;
    L->ox = PAD;
    L->oy = PAD;
    L->radius = card_corner_radius(st);

    U  = rg_phi_unit((float)L->radius);
    U1 = U / RG_PHI;
    U2 = U1 / RG_PHI;
    /* Golden floor: the layout may never get tighter than the icon's own
     * vertical centring margins, (CARD_H - ICON) / 2.  Below that floor the
     * old 6/4/3 minimums let every spacing collapse together with a small
     * corner radius and the whole card read as cramped - the exact
     * "圆角小了排版就不好看" report.  With this floor the slider reshapes
     * the CORNERS while the icon's left / top / bottom margins stay equal at
     * any radius (the 4.9 rule); only past the floor does phi scaling grow
     * the margins further, which is the look the user prefers at maximum. */
    {
        float vmin = ((float)CARD_H - (float)ICON) * 0.5f;
        if (U  < vmin) U  = vmin;
        if (U1 < 8.0f) U1 = 8.0f;
        if (U2 < 5.0f) U2 = 5.0f;
    }

    /* Concentric inset: the icon sits U from the left edge and is vertically
     * centred, which at these proportions is also U from top and bottom.
     * Its own rounding (R - U = U1) is the next concentric step. */
    ICON_X = L->ox + (int)lroundf(U);
    ICON_Y = L->oy + (int)lroundf(((float)CARD_H - (float)ICON) * 0.5f);
    PILL_GAP = (int)lroundf(U2);
    if (PILL_GAP < 4) PILL_GAP = 4;

    L->ht = st->hover ? rg_ease_out(rg_clampf(st->hover_t, 0.0f, 1.0f)) : 0.0f;

    L->text_x = (float)(ICON_X + ICON) + U1;
    L->content_w = fmaxf(60.0f, (float)(PAD + CARD_W) - U - L->text_x);

    L->sub_gap = fmaxf(3.0f, lroundf(5.0f * g_scale));
    L->row_gap = fmaxf(5.0f, lroundf(8.0f * g_scale));

    idle_h = g_title_h + L->sub_gap + g_sub_h;

    /* Anchored on the resting layout.  Hovering swaps the second row in place;
     * re-centring the block for the taller pill row moved the title up by ~10
     * px, which read as the text jumping under the pointer. */
    L->block_top = (float)PAD + lroundf((float)(CARD_H - idle_h) * 0.5f);
    L->row_top = L->block_top + g_title_h +
                 lroundf(L->sub_gap + (L->row_gap - L->sub_gap) * L->ht);

    /* Reserve the title row's right edge for a quiet, permanent menu affordance.
     * Only the title is shortened; subtitle and pills retain their proportions. */
    L->menu_w = scale_k(28, 18);
    L->menu_h = scale_k(26, 18);
    L->menu_x = (int)lroundf((float)(PAD + CARD_W) - U) - L->menu_w;
    L->menu_y = (int)lroundf(L->block_top + (g_title_h - (float)L->menu_h) * 0.5f);
    L->title_w = fmaxf(20.0f, (float)L->menu_x - U2 - L->text_x);

    L->sub_a = rg_clampf(1.0f - L->ht / 0.45f, 0.0f, 1.0f);
    L->pill_a = rg_clampf((L->ht - 0.32f) / 0.68f, 0.0f, 1.0f);
    L->dy = (int)lroundf((1.0f - L->pill_a) * fmaxf(3.0f, lroundf(6.0f * g_scale)));

    L->bw = (int)fminf((float)PILL_MAX_W, floorf((L->content_w - (float)PILL_GAP) * 0.5f));
    if (L->bw < 20)
        L->bw = 20;
    L->total_w = L->bw * 2 + PILL_GAP;
    L->bx0 = (int)lroundf(L->text_x +
                          fmaxf(0.0f, floorf((L->content_w - (float)L->total_w) * 0.5f)));
}

/* --------------------------------------------------------- static layer */

void render_invalidate(void)
{
    g_skey_valid = FALSE;
    g_render_epoch++;
}

/* ------------------------------------------------------------ glass panel */

/* Shadow + blurred backdrop + tint + sheen + rim + border, for an arbitrary
 * box.  Both the card and the context menu go through this, which is why the
 * menu reads as the same material as the widget. */
/* --------------------------------------------------------------- rim layer
 *
 * The light catch along the card edge: a hairline of light hugging the
 * silhouette, brightest along the top, easing down the sides and closing with
 * a faint return along the bottom lip - light falling from above onto glass.
 *
 * It is rasterised once into a cached layer and sampled from a distance field
 * rather than stroked.  A 1.4 px stroke antialiases into a dotted seam at 1x
 * - a string of white specks lying in the card's dark edge - and clipping
 * that stroke to the mask on bright pages cut it into disconnected fragments.
 * Both read as dirt.  With the field, every pixel knows how far it is from the
 * silhouette, so the band stays continuous at any width and its falloff can be
 * shaped instead of being one flat stroke.  The layer never changes while the
 * card is being dragged, so this costs nothing per frame.
 */
/* Position of the falloff along the height, as a fraction of the card.  It
 * reaches zero by 92%: any residual catch on the very bottom row reads as a
 * bright hairline lying across the glass, which is the "gradient boundary"
 * that kept showing up on dark wallpapers. */
static const float RG_RIM_T[6] = { 0.00f, 0.18f, 0.39f, 0.60f, 0.80f, 0.92f };
static const float RG_RIM_K[6] = { 1.00f, 0.90f, 0.58f, 0.51f, 0.30f, 0.00f };

/* Exact signed distance to the rounded box the card mask is built from, in
 * pixels, positive outside.  The corner is a plain arc (SQ_N == 2), so the
 * closed form of a rounded-box field applies and the result is exact rather
 * than a gradient-normalised approximation. */
static float card_sdf(float px, float py, float w, float h, float r)
{
    float hx = w * 0.5f, hy = h * 0.5f;
    float qx, qy, ax, ay, m;

    if (r > hx) r = hx;
    if (r > hy) r = hy;
    if (r < 0.0f) r = 0.0f;
    qx = fabsf(px - hx) - (hx - r);
    qy = fabsf(py - hy) - (hy - r);
    ax = qx > 0.0f ? qx : 0.0f;
    ay = qy > 0.0f ? qy : 0.0f;
    m = qx > qy ? qx : qy;
    return sqrtf(ax * ax + ay * ay) + (m < 0.0f ? m : 0.0f) - r;
}

static float rim_gain(float t)
{
    int i;

    if (t <= RG_RIM_T[0])
        return RG_RIM_K[0];
    for (i = 1; i < 6; i++)
    {
        if (t <= RG_RIM_T[i])
        {
            float u = (t - RG_RIM_T[i - 1]) / (RG_RIM_T[i] - RG_RIM_T[i - 1]);
            return RG_RIM_K[i - 1] + (RG_RIM_K[i] - RG_RIM_K[i - 1]) * u;
        }
    }
    return RG_RIM_K[5];
}

/* Profile of the catch, measured in px of signed distance from the edge.  A
 * hard specular line sits a third of a pixel inside the silhouette; a soft,
 * much wider term is the light scattering in from the glass wall.  Together
 * they read as thickness, which a single stroke cannot. */
#define RIM_SPEC_OFF 0.45f
#define RIM_SPEC_SIG 1.00f
#define RIM_WALL_OFF 2.40f
#define RIM_WALL_SIG 1.85f
#define RIM_WALL_K   0.20f

/* How much light the surface catches, from the direction its edge faces.
 *
 * The old profile was a function of height alone, so the left and right edges
 * of the card were lit exactly like the top, and the ring read as a white
 * stroke drawn around the shape.  Light that falls from straight above is
 * caught by the top edge, grazed by the sides and barely touched by the bottom
 * - and around the corner arcs the amount falls off continuously, which is
 * what makes the catch look like it belongs to the material.  A small floor
 * keeps the lower half from disappearing: a dark card with no edge at all
 * reads as a shadow rather than as glass. */
/* Tight specular lobe: the light source is small, so the catch collapses to a
 * narrow streak where the edge faces it, instead of a band of even width all
 * along the top. */
static float rim_lobe(float ny)
{
    float up = -ny;

    if (up <= 0.0f)
        return 0.0f;
    return powf(up, 2.6f);
}

static float rim_dir(float nx, float ny)
{
    float up = -ny, down = ny, g;

    if (up < 0.0f)
        up = 0.0f;
    if (down < 0.0f)
        down = 0.0f;
    /* 0.45 on the verticals: a glass side wall does catch a lit line, and
     * without it the dark body dissolves into its own shadow and the shape has
     * no silhouette at all.  The bottom keeps almost nothing. */
    g = 0.80f - 0.65f * down + 0.20f * powf(up, 1.40f);
    (void)nx;
    return g < 0.0f ? 0.0f : g;
}

static GpBitmap *rim_layer(int w, int h, int radius, int a0, ARGB border,
                           int style, ARGB accent, ARGB accent_b, int light,
                           float env_gain)
{
    GpBitmap *bmp = NULL;
    BitmapData bd;
    int x, y;
    int brd_a = (int)((border >> 24) & 0xFF);
    /* The catch is white carrying a hint of the material colour.  A full
     * accent tint is what turned the rim into a beige halo on a white page. */
    int tint = light ? 10 : (style == 2 ? 40 : (style == 1 ? 52 : 64));
    int cr, cg, cb, dr, dg, db;
    float s = rg_clampf(g_scale, 0.85f, 2.0f);
    float spec_off = RIM_SPEC_OFF * s;
    float spec_sig = RIM_SPEC_SIG * s;
    float wall_off = RIM_WALL_OFF * s;
    float wall_sig = RIM_WALL_SIG * s;

    if (g_rim && g_rim_w == w && g_rim_h == h && g_rim_r == radius &&
        g_rim_a == a0 && g_rim_brd == (int)border && g_rim_style == style &&
        g_rim_accent == accent && g_rim_accent_b == accent_b &&
        g_rim_light == light && g_rim_env == env_gain)
        return g_rim;

    if (g_rim)
    {
        GdipDisposeImage(g_rim);
        g_rim = NULL;
    }
    g_rim_w = w;
    g_rim_h = h;
    g_rim_r = radius;
    g_rim_a = a0;
    g_rim_brd = (int)border;
    g_rim_style = style;
    g_rim_accent = accent;
    g_rim_accent_b = accent_b;
    g_rim_light = light;
    g_rim_env = env_gain;

    if (a0 <= 0 && brd_a <= 0)
        return NULL;

    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB, NULL, &bmp) != Ok)
        return NULL;

    cr = 255 + (int)(((int)((accent >> 16) & 0xFF) - 255) * tint / 255);
    cg = 255 + (int)(((int)((accent >> 8) & 0xFF) - 255) * tint / 255);
    cb = 255 + (int)(((int)(accent & 0xFF) - 255) * tint / 255);
    /* Second material stop for the lower half: the catch carries the same
     * two-tone dialogue as the body instead of one flat white. */
    dr = 255 + (int)(((int)((accent_b >> 16) & 0xFF) - 255) * tint / 255);
    dg = 255 + (int)(((int)((accent_b >> 8) & 0xFF) - 255) * tint / 255);
    db = 255 + (int)(((int)(accent_b & 0xFF) - 255) * tint / 255);

    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(bmp, NULL, ImageLockModeRead | ImageLockModeWrite,
                           PixelFormat32bppPARGB, &bd) != Ok)
    {
        GdipDisposeImage(bmp);
        return NULL;
    }

    for (y = 0; y < h; y++)
    {
        /* Scattering inside the wall follows the same light as the catch, so
         * the glow is strongest where the light enters: along the top. */
        float wt = h > 1 ? (float)y / (float)(h - 1) : 0.0f;
        float wall_vert = 0.30f + 0.70f * rim_gain(h > 1 ? wt : 1.0f);
        float bt = rg_clampf(wt * 1.35f, 0.0f, 1.0f);
        int rr = (int)((float)cr + ((float)dr - (float)cr) * bt);
        int rg = (int)((float)cg + ((float)dg - (float)cg) * bt);
        int rb = (int)((float)cb + ((float)db - (float)cb) * bt);
        BYTE *row = (BYTE *)bd.Scan0 + (size_t)y * bd.Stride;

        for (x = 0; x < w; x++)
        {
            BYTE *q = row + x * 4;
            float d = card_sdf((float)x + 0.5f, (float)y + 0.5f,
                               (float)w, (float)h, (float)radius);
            float spec, wall, as, ab, oa, dir, lobe, nx, ny, nl, lr, lg, lb;

            if (d > 2.4f * s)
                continue;
            /* Normal from the field itself: one central difference per axis.
             * Only the few pixels of the band pay for it. */
            nx = card_sdf((float)x + 1.5f, (float)y + 0.5f, (float)w, (float)h, (float)radius) -
                 card_sdf((float)x - 0.5f, (float)y + 0.5f, (float)w, (float)h, (float)radius);
            ny = card_sdf((float)x + 0.5f, (float)y + 1.5f, (float)w, (float)h, (float)radius) -
                 card_sdf((float)x + 0.5f, (float)y - 0.5f, (float)w, (float)h, (float)radius);
            nl = sqrtf(nx * nx + ny * ny);
            dir = nl > 1e-4f ? rim_dir(nx / nl, ny / nl) : 1.0f;
            lobe = nl > 1e-4f ? rim_lobe(ny / nl) : 1.0f;
            /* The catch is a band where the surface faces the light and a
             * hairline where it only grazes it: the width follows the same
             * direction term as the strength, which is what stops the rim
             * from reading as one even stroke around the shape. */
            /* Over a wallpaper the catch stays a hairline: there is already
             * light behind the glass, and a wide white band on a colourful
             * desktop reads as a drawn border. */
            {
                float sw = spec_sig * (0.75f + 0.30f * dir) * (light ? 1.0f : 0.78f);
                spec = expf(-0.5f * ((d + spec_off) / sw) * ((d + spec_off) / sw));
            }
            /* Light that crossed the slab and bounced once off the far face:
             * a dim second line just inside the bottom edge.  Without it the
             * glass reads as a painted shell - this is the cheapest cue that
             * there is something behind the front surface. */
            {
                float bounce_y = ny > 0.0f ? ny : 0.0f;
                float bd_off = 3.4f * s;
                wall = RIM_WALL_K * wall_vert *
                       expf(-0.5f * ((d + wall_off) / wall_sig) *
                            ((d + wall_off) / wall_sig)) +
                       0.34f * bounce_y * bounce_y *
                       expf(-0.5f * ((d + bd_off) / (1.3f * s)) *
                            ((d + bd_off) / (1.3f * s)));
            }
            if (light)
            {
                /* On paper the edge cannot be told by light: a white hairline
                 * on white is invisible.  A real glass edge reads as a dark
                 * contour where the surface grazes the light and as a white
                 * catch where it faces it, so the alpha keeps a floor and the
                 * colour swings instead. */
                as = (spec + wall) * (0.28f + 0.72f * dir) * (float)a0;
            }
            else
            {
                /* The catch follows the light source, not the height: a tight
                 * lobe where the edge faces the light, the old even term for
                 * the sides so the silhouette still closes. */
                as = (spec + wall) * (0.72f * dir + 0.55f * lobe) * (float)a0;
            }
            as *= env_gain;
            if (d > 0.0f)
            {
                /* Outside the silhouette only a half-pixel bloom survives.  On
                 * a bright page anything wider reads as a pale halo around the
                 * card; on a wallpaper it is the light the glass catches. */
                float cut = rg_clampf((1.45f * s - d) / (1.1f * s), 0.0f, 1.0f);
                as *= (light ? 0.45f : 0.55f) * cut;
            }
            ab = brd_a > 0
                 ? (float)brd_a * expf(-0.5f * ((d + 0.28f * s) / (0.62f * s)) *
                                       ((d + 0.28f * s) / (0.62f * s)))
                 : 0.0f;
            if (as < 0.4f && ab < 0.4f)
                continue;
            if (as > 255.0f)
                as = 255.0f;
            {
                /* The catch loses its whiteness where the light only grazes the
                 * surface.  Keeping it white all the way round is what makes a
                 * rim look like a stroke drawn on top of the card; dimming the
                 * colour towards neutral at the sides lets the edge stay lit
                 * without turning into an outline. */
                if (light)
                {
                    float w = 0.25f * dir + 0.75f * lobe;
                    lr = 176.0f + (255.0f - 176.0f) * w;
                    lg = 178.0f + (255.0f - 178.0f) * w;
                    lb = 184.0f + (255.0f - 184.0f) * w;
                }
                else
                {
                    float k = 0.66f + 0.34f * (0.45f * dir + 0.55f * lobe);
                    lr = (float)rr * k;
                    lg = (float)rg * k;
                    lb = (float)rb * k;
                }
                /* A lens splits the catch by a hair: warm on the outside of the
                 * band, cool on the inside.  Two levels of colour separation,
                 * enough to read as glass rather than as white paint. */
                {
                    /* Only inside the catch.  Applied everywhere it turned
                     * into coloured specks along the lower edge, where the
                     * band is a dark contour with almost no signal. */
                    float disp = rg_clampf((d + spec_off) * 10.0f * lobe,
                                           -16.0f, 16.0f);
                    lr += disp;
                    lb -= disp;
                }
            }
            /* The border sits under the catch, so the two never sum to a
             * brighter-than-white seam. */
            oa = as + ab * (1.0f - as / 255.0f);
            if (oa > 255.0f)
                oa = 255.0f;
            q[0] = (BYTE)((lb * as + (float)(border & 0xFF) * ab *
                           (1.0f - as / 255.0f)) / 255.0f);
            q[1] = (BYTE)((lg * as + (float)((border >> 8) & 0xFF) * ab *
                           (1.0f - as / 255.0f)) / 255.0f);
            q[2] = (BYTE)((lr * as + (float)((border >> 16) & 0xFF) * ab *
                           (1.0f - as / 255.0f)) / 255.0f);
            q[3] = (BYTE)(oa + 0.5f);
        }
    }
    GdipBitmapUnlockBits(bmp, &bd);

    g_rim = bmp;
    return g_rim;
}

static int backdrop_channel(const BYTE *src, size_t stride, int w, int h,
                            float x, float y, int channel)
{
    int x0, y0, x1, y1;
    float tx, ty, top, bottom;
    const BYTE *p00, *p10, *p01, *p11;

    x = rg_clampf(x, 0.0f, (float)(w - 1));
    y = rg_clampf(y, 0.0f, (float)(h - 1));
    x0 = (int)x;
    y0 = (int)y;
    x1 = x0 + 1 < w ? x0 + 1 : x0;
    y1 = y0 + 1 < h ? y0 + 1 : y0;
    tx = x - (float)x0;
    ty = y - (float)y0;

    p00 = src + (size_t)y0 * stride + (size_t)x0 * 4;
    p10 = src + (size_t)y0 * stride + (size_t)x1 * 4;
    p01 = src + (size_t)y1 * stride + (size_t)x0 * 4;
    p11 = src + (size_t)y1 * stride + (size_t)x1 * 4;
    top = (float)p00[channel] + ((float)p10[channel] - p00[channel]) * tx;
    bottom = (float)p01[channel] + ((float)p11[channel] - p01[channel]) * tx;
    return rg_clampi((int)lroundf(top + (bottom - top) * ty), 0, 255);
}

/* Set for the card while it is inside the capture (see RgRenderState::
 * unblend_body).  An opaque body cannot be un-composited - (c - f) / (1 - a)
 * divides by zero - so the card's own picture would survive in the backdrop
 * and the glass would show a blurred copy of itself.  Keeping the body a few
 * percent transparent gives the inversion real signal while looking identical
 * (the backdrop behind the glass is already baked in). */
static int g_unblend_body;

static void paint_glass(GpGraphics *g, const RgBitmap *bd, const RgBitmap *rim,
                        int x, int y, int w, int h, int radius,
                        float opacity, float material, int drag_hover,
                        const Palette *p, int dark, int glass_style, int accent,
                        float refraction, float coverage, int flat)
{
    float fac = rg_clampf(p->opacity_floor + p->opacity_span * opacity,
                          0.0f, 1.0f);
    int style = style_index(glass_style);
    float refr = rg_clampf(refraction, 0.0f, 1.0f) *
                 (style == 0 ? 1.0f : (style == 1 ? 0.42f : 0.22f));
    const BYTE *mask;
    int bd_lum = -1;   /* backdrop luminance, -1 = unknown; drives the rim tint */

    /* The screenshot body has to read on a white document as well as on a dark
     * wallpaper, and it cannot borrow contrast from a backdrop that may be
     * missing.  The opacity slider still works above this floor. */
    if (flat && fac < 0.74f)
        fac = 0.74f;

    /* 1. shadow */
    {
        GpBitmap *sh = shadow_sprite(w, h, radius,
                                     rg_clampi((int)((float)p->shadow_alpha *
                                                     (0.55f + 0.45f * fac)), 0, 255));
        if (sh)
            GdipDrawImageRectI(g, sh, x - SHADOW_PAD, y - SHADOW_PAD + SHADOW_DY,
                               w + SHADOW_PAD * 2, h + SHADOW_PAD * 2);
    }

    /* 2. glass body */
    {
        size_t need = (size_t)w * h * 4;
        if (need > g_body_cap)
        {
            BYTE *nb = (BYTE *)realloc(g_body, need);
            if (nb)
            {
                g_body = nb;
                g_body_cap = need;
                g_body_gen++;       /* the cached wrapper now points at freed memory */
            }
        }
    }

    mask = card_mask(w, h, radius);

    if (g_body && mask)
    {
        /* Everything below is fixed-point integer maths.  This loop runs over
         * every pixel of the card on every frame of a drag; the float version
         * cost more than the rest of the card put together. */
        /* The 透明度 slider is MATERIAL DENSITY, not window alpha: it scales
         * how much of the material colour is mixed into the blurred backdrop,
         * from colourless glass (0) to the full substance (1).  The body stays
         * present so the card never "fades out" as a whole, and text, icons and
         * buttons keep their own full opacity. */
        int dens   = (int)(rg_clampf(fac, 0.0f, 1.0f) * 256.0f + 0.5f);
        int tint_a = (int)(rg_clampf(p->tint_floor + p->tint_span * material,
                                     0.0f, 1.0f) * 256.0f + 0.5f);
        tint_a = (tint_a * dens) >> 8;
        /* A menu is READ, not glanced at.  A clean capture now gives us the
         * real desktop: behind a white window that would leave white-on-white
         * text with the dark theme.  Keep a minimum tint toward the theme
         * material whenever the surface is baked opaque (coverage > 0), so
         * contrast is guaranteed on any background.  The decorative card
         * (coverage <= 0) keeps its full transparency range. */
        if ((coverage > 0.0f || flat) && tint_a < 200)
            tint_a = 200;
        /* `fac` still drives the tint and the rim.  Coverage is separate: a
         * frosted panel must be nearly opaque or the SHARP desktop behind it
         * leaks through and the blur is invisible.  Apple's menu materials
         * work the same way - the blurred backdrop is baked into the surface,
         * not left as a half-transparent overlay.  coverage <= 0 keeps the
         * card's original "alpha follows the opacity slider" behaviour. */
        int fac8   = coverage > 0.0f
                     ? (int)(rg_clampf(coverage, 0.0f, 1.0f) * 256.0f + 0.5f)
                     : 256;   /* card body: always present, density is tint */

        /* Leave ~7% of the composited screen visible through the body so
         * self_unblend() has something to solve for.  232/256 is 0.906: the
         * card looks the same (its backdrop is baked into these pixels) and
         * the inversion gets 24 levels of signal instead of zero. */
        if (g_unblend_body && !flat && fac8 > 238)
            fac8 = 238;
        int drag8  = 0;
        int drag_r = 0, drag_g = 0, drag_b = 0;
        int sheen_top = style == 2 ? p->sheen_top * 3 / 4 : p->sheen_top;
        int bottom_shade = style == 2 ? p->bottom_shade * 3 / 4 : p->bottom_shade;
        /* The band must stay well inside the corner arc, otherwise its inner
         * boundary at the corners collapses to a 3-5 px radius - which reads
         * as a square right-angle ring around the whole shape (the exact
         * artefact from the 4.5.0 screenshots).  0.45x keeps a visible arc. */
        /* The band has to follow the displacement, not the corner radius: a
         * big radius used to stretch it over the whole end cap, which is how
         * the corners turned into a solid smeared block (4.15.5). */
        float refr_band = rg_clampf(((3.0f + 16.0f * g_scale) * refr) * 1.15f,
                                    7.0f, 30.0f);
        /* The bend has to stay inside the band.  3 + 26*scale sampled up to
         * ~34 px outward at 150% - beyond two 16 px grid cells - and read as
         * the glass pulling the desktop in from too far away.  2 + 10*scale
         * tops out at ~13 px (plus ~2 px of dispersion), i.e. under 0.6x of
         * the 26 px band: still an obvious lens at 100%, and the sample can
         * never reach past the captured margin at 200%. */
        /* Twice the old travel: the slider used to top out at 15.5 px of bend
         * on a 340 px card, which reads as a hairline lens rather than a slab. */
        float refr_depth = (3.0f + 13.0f * g_scale) * refr;
        /* A plane-parallel slab also shifts the whole image sideways by
         * thickness * (1 - 1/n).  Without it the interior never moves and the
         * glass is a bent rim pasted on a flat picture. */
        float refr_par = (1.0f + 3.0f * g_scale) * refr;
        int par_x = (int)lroundf(refr_par * 0.35f);
        int par_y = (int)lroundf(refr_par * 0.90f);
        int bd_w = bd ? bd->w : 0, bd_h = bd ? bd->h : 0;
        int bd_ox = 0, bd_oy = 0;
        int mist_veil = style == 2 ? 7 : 0;
        ARGB accent_a = accent_from(dark, accent, 255);
        ARGB accent_b = accent_to(dark, accent, 255);
        int a0r = (accent_a >> 16) & 0xFF, a0g = (accent_a >> 8) & 0xFF;
        int a0b = accent_a & 0xFF;
        int a1r = (accent_b >> 16) & 0xFF, a1g = (accent_b >> 8) & 0xFF;
        int a1b = accent_b & 0xFF;
        const BYTE *src = NULL;
        size_t src_stride = 0;
        const BYTE *rsrc = NULL;
        size_t rstride = 0;
        int px, py;

        /* Mist lowers contrast without changing the meaning of the material
         * slider; Crystal and Aurora retain its original response. */
        if (style == 2)
            tint_a += ((256 - tint_a) * 12) / 100;

        if (drag_hover)
        {
            ARGB dc = blend_argb(accent_from(dark, accent, 255),
                                 accent_to(dark, accent, 255), 0.5f);
            drag8 = (p->drag_wash >> 24) & 0xFF;
            drag_r = (dc >> 16) & 0xFF;
            drag_g = (dc >> 8) & 0xFF;
            drag_b = dc & 0xFF;
        }

        if (bd && bd->px && bd->w >= w && bd->h >= h)
        {
            src = bd->px;
            src_stride = (size_t)bd->w * 4;
            if (bd->w >= w + REFR_PAD * 2 && bd->h >= h + REFR_PAD * 2)
            {
                bd_ox = REFR_PAD;
                bd_oy = REFR_PAD;
            }
            /* The rim is the unblurred source of the same crop; only usable
             * when its geometry matches, otherwise the sample coordinates
             * would land in the wrong place. */
            if (rim && rim->px && rim->w == bd->w && rim->h == bd->h)
            {
                rsrc = rim->px;
                rstride = (size_t)rim->w * 4;
            }
        }

        /* The lateral shift samples past the card edge, so it is only safe
         * when the capture actually carries the margin around it. */
        if (bd_ox <= 0 && bd_oy <= 0)
        {
            par_x = 0;
            par_y = 0;
        }

        /* Contrast floor that follows the backdrop.
         *
         * The tint is a blend, not an opacity: at a fixed material level a dark
         * card over a white document lands around #6D6D79, and the page text
         * behind it stays legible right through the title.  Apple's materials
         * solve this by getting denser over bright content, and so does this -
         * the tint is solved so the body keeps a TARGET luminance instead of a
         * target tint.  It changes COLOUR only: the body alpha stays at the
         * 0.906 that self_unblend() needs, so the card stays recoverable from
         * the composite and still shows what is behind it.
         *
         * The low end is untouched (a dark wallpaper keeps its original
         * material), which is why the switch costs nothing on the desktop. */
        if (src && bd_w > 0 && bd_h > 0)
        {
            int si, sn = 0, sl = 0;
            int step = (bd_w * bd_h) / 512 + 1;
            int tint_lum = (p->tint_r * 77 + p->tint_g * 151 + p->tint_b * 28) >> 8;

            for (si = 0; si < bd_w * bd_h; si += step)
            {
                const BYTE *q = src + (size_t)si * 4;
                sl += (q[2] * 77 + q[1] * 151 + q[0] * 28) >> 8;
                sn++;
            }
            if (sn)
            {
                int lum = sl / sn;
                int target = dark ? 44 : 236;
                int t = 0;

                bd_lum = lum;

                if (dark && lum > target)
                    t = (lum - target) * 256 / (lum - tint_lum + 1);
                else if (!dark && lum < target)
                    t = (target - lum) * 256 / (tint_lum - lum + 1);
                /* Scaled by the MATERIAL slider, not by the dead opacity one.
                 *
                 * This floor exists only to keep text readable when the glass
                 * is dense.  Applied at full strength it made the body jump to
                 * the target luminance no matter where the material slider
                 * was, so "0" was not transparent at all - and because it only
                 * fires when the backdrop is darker than the target, the card
                 * looked clear over a bright window and milky white over the
                 * wallpaper.  That single term is the "only works over
                 * windows" report. */
                t = (t * (int)(rg_clampf(material, 0.0f, 1.0f) * 256.0f)) >> 8;
                if (t > 256)
                    t = 256;
                if (t > tint_a)
                    tint_a = t;
            }
        }

        for (py = 0; py < h; py++)
        {
            /* The sheen is light scattered inside the material, so it scales
             * with it.  At full strength on clear glass it laid a 46% white
             * veil over the top half - the card read as frosted white even
             * with the material slider at zero. */
            int sh = sheen_alpha(py, h, sheen_top, sheen_top / 3, bottom_shade);
            sh = (sh * (int)(rg_clampf(material, 0.0f, 1.0f) * 256.0f)) >> 8;
            BYTE *dst = g_body + (size_t)py * w * 4;
            const BYTE *sp = src ? src + (size_t)(py + bd_oy + par_y) * src_stride +
                                   (size_t)(bd_ox + par_x) * 4 : NULL;
            const BYTE *mp = mask + (size_t)py * w;

            for (px = 0; px < w; px++, dst += 4, mp++)
            {
                int r, g, b, a;
                int tint_local = tint_a;

                if (sp)
                {
                    b = sp[0];
                    g = sp[1];
                    r = sp[2];
                    sp += 4;
                }
                else
                {
                    r = p->tint_r;
                    g = p->tint_g;
                    b = p->tint_b;
                }

                /* The glass bends what is actually around it.  Samples point
                 * outward into the captured margin, so the rim shows a
                 * compressed, chromatic view of the desktop instead of a smear
                 * of the card's own interior.  R and B travel slightly
                 * different distances for restrained dispersion. */
                if (src && refr > 0.0f)
                {
                    /* Rounded-box signed distance.  The old min(dx,dy)
                     * metric turned the corner band into a 45-degree chamfer
                     * - a straight cut that read as a "sharp bend" exactly
                     * where the arc should be.  The true SDF keeps the band
                     * and its normal concentric with the corner arc, so the
                     * glass wraps like one solid piece of crystal. */
                    float hw = (float)w * 0.5f - 0.5f;
                    float hh = (float)h * 0.5f - 0.5f;
                    float cr = (float)radius;
                    float qx = fabsf((float)px - hw) - (hw - cr);
                    float qy = fabsf((float)py - hh) - (hh - cr);
                    float ox = qx > 0.0f ? qx : 0.0f;
                    float oy = qy > 0.0f ? qy : 0.0f;
                    float dist_in = cr - (sqrtf(ox * ox + oy * oy) +
                                          fminf(fmaxf(qx, qy), 0.0f));
                    float k = 1.0f - dist_in / refr_band;

                    if (k > 1.0f)
                        k = 1.0f;

                    if (k > 0.0f)
                    {
                        float sx = (float)px < hw ? -1.0f : 1.0f;
                        float sy = (float)py < hh ? -1.0f : 1.0f;
                        float nx, ny, nl;
                        float bend = k * sqrtf(k);
                        float amount = refr_depth * bend;
                        /* No per-channel offset here.  Splitting the sample by
                         * colour also splits the band's own gradient, and over
                         * a shadowed edge that prints as cyan/magenta rows
                         * several levels wide - visible dirt on a flat page.
                         * The chromatic edge lives in the catch instead (see
                         * rim_layer), where it is 1 px of a bright line. */
                        float split = 0.0f;
                        float bx = (float)(bd_ox + px + par_x);
                        float by = (float)(bd_oy + py + par_y);

                        nx = sx * ox;
                        ny = sy * oy;
                        nl = sqrtf(nx * nx + ny * ny);
                        if (nl > 1e-4f)
                        {
                            nx /= nl;
                            ny /= nl;
                        }

                        r = backdrop_channel(src, src_stride, bd_w, bd_h,
                                             bx + nx * (amount + split),
                                             by + ny * (amount + split), 2);
                        g = backdrop_channel(src, src_stride, bd_w, bd_h,
                                             bx + nx * amount,
                                             by + ny * amount, 1);
                        b = backdrop_channel(src, src_stride, bd_w, bd_h,
                                             bx + nx * (amount - split),
                                             by + ny * (amount - split), 0);

                        /* A blurred plane loses all local contrast once the
                         * radius swallows the features, so the bend above has
                         * nothing left to refract and the rim goes flat at high
                         * blur values.  The band is exactly where a real lens
                         * shows the sharp surroundings, so mix the unblurred
                         * crop back in there, fading to pure blurred toward the
                         * inner edge to keep the seam invisible. */
                        if (rsrc)
                        {
                            float sk = bend * 1.25f;
                            int sr, sg, sb;

                            if (sk > 1.0f)
                                sk = 1.0f;
                            sr = backdrop_channel(rsrc, rstride, bd_w, bd_h,
                                                  bx + nx * (amount + split),
                                                  by + ny * (amount + split), 2);
                            sg = backdrop_channel(rsrc, rstride, bd_w, bd_h,
                                                  bx + nx * amount,
                                                  by + ny * amount, 1);
                            sb = backdrop_channel(rsrc, rstride, bd_w, bd_h,
                                                  bx + nx * (amount - split),
                                                  by + ny * (amount - split), 0);
                            r += (int)((float)(sr - r) * sk);
                            g += (int)((float)(sg - g) * sk);
                            b += (int)((float)(sb - b) * sk);
                        }

                        /* A thin lit meniscus sells the thickness of the glass;
                         * its width follows the same slider, so the control has
                         * a visible effect even on a flat wallpaper. */
                        {
                            int bevel = (int)(6.0f * bend * refr);
                            r += bevel;
                            g += bevel;
                            b += bevel;
                        }

                        /* Light crosses less glass at the rim, so the band keeps
                         * more of the real surroundings instead of being dyed
                         * as heavily as the centre.  Without this the bend is
                         * there but the material tint hides it. */
                        tint_local = tint_a -
                                     (int)((float)tint_a * 0.62f * bend * refr);
                    }
                }

                /* tint towards the material colour */
                r += ((p->tint_r - r) * tint_local) >> 8;
                g += ((p->tint_g - g) * tint_local) >> 8;
                b += ((p->tint_b - b) * tint_local) >> 8;

                /* A diagonal two-stop tint makes the body read as dyed glass
                 * rather than a flat translucent plate.  Aurora keeps a much
                 * stronger field; crystal and mist get only a breath of colour
                 * so their material differences stay visible. */
                {
                    float ax = (float)px / (float)(w > 1 ? w - 1 : 1);
                    float ay = (float)py / (float)(h > 1 ? h - 1 : 1);
                    float bt = rg_clampf(ax * 0.64f + ay * 0.36f, 0.0f, 1.0f);
                    int ar = (int)rg_lerp((float)a0r, (float)a1r, bt);
                    int ag = (int)rg_lerp((float)a0g, (float)a1g, bt);
                    int ab = (int)rg_lerp((float)a0b, (float)a1b, bt);
                    float top = rg_clampf(1.0f - ay / 0.58f, 0.0f, 1.0f);
                    int mix = style == 0 ? (14 + (int)(top * top * 20.0f))
                                         : (7 + (int)(top * top * 10.0f));

                    if (style == 1)
                    {
                        float glow = rg_clampf(1.0f - fabsf(ax * 1.28f - ay - 0.12f),
                                               0.0f, 1.0f);
                        mix = (int)(glow * glow * 62.0f) +
                              (int)(top * top * 18.0f);
                    }
                    /* The two-stop accent field is part of the material too:
                     * at material 0 it was still laying a 5-13% colour wash
                     * over clear glass. */
                    mix = (mix * (int)(rg_clampf(material, 0.0f, 1.0f) * 256.0f)) >> 8;
                    if (mix > 0)
                    {
                        r += ((ar - r) * mix) >> 8;
                        g += ((ag - g) * mix) >> 8;
                        b += ((ab - b) * mix) >> 8;
                    }
                }

                if (style != 1 && mist_veil)
                {
                    r += ((255 - r) * mist_veil) >> 8;
                    g += ((255 - g) * mist_veil) >> 8;
                    b += ((255 - b) * mist_veil) >> 8;
                }

                /* top sheen / bottom shade */
                if (sh > 0)
                {
                    r += ((255 - r) * sh) >> 8;
                    g += ((255 - g) * sh) >> 8;
                    b += ((255 - b) * sh) >> 8;
                }
                else if (sh < 0)
                {
                    int k = 256 + sh;
                    r = (r * k) >> 8;
                    g = (g * k) >> 8;
                    b = (b * k) >> 8;
                }

                /* A fixed, monochrome +/-1 grain prevents temporal sparkle and
                 * avoids the coloured speckle that makes frosted glass look dirty. */
                if (style == 2 && !flat)
                {
                    unsigned hash = (unsigned)px * 0x9E3779B1u ^
                                    (unsigned)py * 0x85EBCA6Bu;
                    int grain;
                    hash ^= hash >> 16;
                    grain = (int)(hash & 3u) - 1;
                    r = rg_clampi(r + grain, 0, 255);
                    g = rg_clampi(g + grain, 0, 255);
                    b = rg_clampi(b + grain, 0, 255);
                }
                else if (flat && !src)
                {
                    /* No captured pixels means no natural detail to hide the
                     * quantisation of the tint - a bare gradient banded visibly
                     * against a flat wallpaper.  A 2x2-cell +/-2 frost grain is
                     * the cheapest honest substitute: it reads as diffused
                     * surface, not as noise, and it is deterministic so the
                     * still card never shimmers. */
                    unsigned hash = (unsigned)(px >> 1) * 0x9E3779B1u ^
                                    (unsigned)(py >> 1) * 0x85EBCA6Bu;
                    int grain;
                    hash ^= hash >> 15;
                    grain = (int)(hash % 5u) - 2;
                    r = rg_clampi(r + grain, 0, 255);
                    g = rg_clampi(g + grain, 0, 255);
                    b = rg_clampi(b + grain, 0, 255);
                }

                /* wash while dragging */
                if (drag8)
                {
                    r += ((drag_r - r) * drag8) >> 8;
                    g += ((drag_g - g) * drag8) >> 8;
                    b += ((drag_b - b) * drag8) >> 8;
                }

                /* A constant inner hairline.
                 *
                 * Everything else at the rim is derived from the backdrop, so
                 * its brightness tracks whatever is behind the card: a white
                 * page lights the ring up, a paragraph of text cuts it into
                 * dashes, and the outline reads as blotchy instead of one
                 * smooth curve.  A fixed 1 px stroke that only depends on the
                 * coverage gives the silhouette an even, continuous edge on any
                 * background, while the refraction band above still supplies
                 * the glassy bend. */
                /* The refracting rim adds light on purpose, so the channels
                 * can leave 0..255 before premultiplication; wrapping here
                 * produced dark speckles on bright wallpapers. */
                r = rg_clampi(r, 0, 255);
                g = rg_clampi(g, 0, 255);
                b = rg_clampi(b, 0, 255);

                a = (*mp * fac8) >> 8;
                if (a <= 0)
                {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                if (a > 255)
                    a = 255;

                /* Undo the transparency leak.
                 *
                 * The body is capped at 0.906 so self_unblend() can solve it
                 * back out of the composited screen.  The visible cost of that
                 * cap is the 9.4% of the SHARP screen that shows through: over
                 * a white document it lifts the body and makes the page text
                 * under the glass far more legible than the blur should allow.
                 * Solving the same compositing equation forwards removes it
                 * exactly - the screen shows what an opaque card would show,
                 * while the surface itself stays invertible. */
                /* Only where the mask is solid.  The inversion solves for the
                 * colour that would produce this pixel over the SHARP screen,
                 * which is true for a fully covered pixel; applied to the
                 * antialiased fringe it subtracts a backdrop that is not
                 * actually there and divides by a fraction, and the result -
                 * composited over the shadow instead of the page - prints as a
                 * dark outline around the whole card. */
                if (g_unblend_body && *mp > 250 && a < 255)
                {
                    const BYTE *sharp = NULL;

                    if (rsrc)
                        sharp = rsrc + (size_t)(py + bd_oy) * rstride +
                                (size_t)(px + bd_ox) * 4;
                    else if (src)
                        sharp = src + (size_t)(py + bd_oy) * src_stride +
                                (size_t)(px + bd_ox) * 4;
                    if (sharp)
                    {
                        int inv = 255 - a;
                        int pr = sharp[2], pg = sharp[1], pb = sharp[0];

                        r = rg_clampi((r * 255 - inv * pr) / a, 0, 255);
                        g = rg_clampi((g * 255 - inv * pg) / a, 0, 255);
                        b = rg_clampi((b * 255 - inv * pb) / a, 0, 255);
                    }
                }

                /* premultiplied */
                dst[0] = (BYTE)((b * a) >> 8);
                dst[1] = (BYTE)((g * a) >> 8);
                dst[2] = (BYTE)((r * a) >> 8);
                dst[3] = (BYTE)a;
            }
        }

        {
            /* Reuse the wrapper: recreating it every frame allocated on the
             * GDI+ heap 200+ times a second. */
            GpBitmap *body_bmp = (GpBitmap *)g_body_bmp;

            if (!body_bmp || g_body_bmp_gen != g_body_gen ||
                g_body_w != w || g_body_h != h)
            {
                if (body_bmp)
                    GdipDisposeImage((GpImage *)body_bmp);
                body_bmp = NULL;
                if (GdipCreateBitmapFromScan0(w, h, w * 4,
                                              PixelFormat32bppPARGB,
                                              g_body, &body_bmp) == Ok)
                {
                    g_body_w = w;
                    g_body_h = h;
                    g_body_bmp_gen = g_body_gen;
                }
                else
                {
                    body_bmp = NULL;
                }
                g_body_bmp = body_bmp;
            }
            if (body_bmp)
                GdipDrawImageRectI(g, body_bmp, x, y, w, h);
        }
    }

    /* 3. rim + border (cached layer, see rim_layer) */
    {
        float style_gain = style == 2 ? 0.68f : (style == 0 ? 1.08f : 1.0f);
        /* The card's rim follows the body into transparency.  A menu keeps a
         * hairline at any slider position - it is read, not glanced at. */
        float rim_fac = coverage > 0.0f ? fmaxf(fac, 0.55f) : fac;
        int rim_a = rg_clampi((int)(p->rim_top_a * rim_fac * style_gain), 0, 255);
        /* Over a light page the edge needs a touch more light to read as glass
         * rather than as a dark cut-out.  It used to be doubled to cover the
         * stitched fringe; the distance-field rim no longer produces one, so
         * a quarter more is enough and it stays off the "white seam" look. */
        if (bd_lum > 148)
            rim_a = rg_clampi(rim_a * 155 / 100, 0, 255);
        /* The catch is a reflection of what is around the glass, so it dims
         * in a dark room instead of staying at a fixed white.  Quantised to
         * four buckets: the layer is cached on this value, and a continuous
         * key would rebuild it on every frame of a drag. */
        float env = 1.0f;
        if (bd_lum >= 0)
            env = 0.80f + 0.20f * rg_clampf((float)bd_lum / 180.0f, 0.0f, 1.0f);
        env = floorf(env * 4.0f + 0.5f) / 4.0f;
        GpBitmap *rl = rim_layer(w, h, radius, rim_a, p->border,
                                 style, accent_from(dark, accent, 255),
                                 accent_to(dark, accent, 255),
                                 bd_lum > 148, env);
        if (rl)
            GdipDrawImageRectI(g, rl, x, y, w, h);
    }
}

void render_glass_panel(RgSurface *s, const RgBitmap *backdrop,
                        const RgBitmap *rim,
                        int x, int y, int w, int h, int radius,
                        float opacity, float material, int dark,
                        int glass_style, int accent, float refraction,
                        float coverage)
{
    const Palette *p = dark ? &PAL_DARK : &PAL_LIGHT;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;

    if (!s || !s->bits || w <= 2 || h <= 2)
        return;

    if (GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4, PixelFormat32bppPARGB,
                                  s->bits, &bmp) != Ok)
        return;

    if (GdipGetImageGraphicsContext(bmp, &g) != Ok || !g)
    {
        GdipDisposeImage(bmp);
        return;
    }
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetCompositingMode(g, CompositingModeSourceOver);

    paint_glass(g, backdrop, rim, x, y, w, h, radius, opacity, material, 0, p,
                dark, glass_style, accent, refraction, coverage, 0);

    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);
}

void render_theme(int dark, int accent, RgTheme *out)
{
    const Palette *p = dark ? &PAL_DARK : &PAL_LIGHT;
    ARGB ac;

    if (!out)
        return;
    ac = accent_argb(dark, accent, 255);

    out->text        = ARGB_OF(255, p->text_r, p->text_g, p->text_b);
    out->dim         = ARGB_OF(255, p->dim_r, p->dim_g, p->dim_b);
    out->accent      = ac;
    out->accent_soft = accent_argb(dark, accent, 38);
    out->accent_alt  = accent_to(dark, accent, 255);
    out->accent_alt_soft = accent_to(dark, accent, 38);
    out->border      = p->border;
    out->hover_bg    = dark ? ARGB_OF(30, 255, 255, 255) : ARGB_OF(20, 0, 0, 0);
    out->track       = dark ? ARGB_OF(46, 255, 255, 255) : ARGB_OF(28, 0, 0, 0);
    out->pill_bg     = p->pill_bg;
    out->pill_text   = p->pill_text;
    out->switch_off  = dark ? ARGB_OF(72, 255, 255, 255) : ARGB_OF(40, 0, 0, 0);
    out->switch_on   = accent_from(dark, accent, 255);
}

/* The part of the card that ignores the backdrop: icon and title.  Kept in a
 * cached premultiplied layer so a moving frame does not re-run GDI+ text. */
static void build_content(const RgRenderState *st, GpGraphics *g, const Layout *L)
{
    const Palette *p = st->dark ? &PAL_DARK : &PAL_LIGHT;

    /* 4. icon, tinted to the current accent unless the original chip is asked
     * for.  The tray copy is generated from the same bitmap (see
     * render_make_icon), so card, tray and menu never disagree. */
    {
        GpImage *ico = st->icon_style == 1 ? g_icon :
                       g_icon_variant[(st->dark ? ACCENT_COUNT : 0) +
                                      accent_index(st->accent)];
        if (!ico)
            ico = g_icon;
        if (ico)
            GdipDrawImageRectI(g, ico, ICON_X, ICON_Y, ICON, ICON);
    }

    /* 5. title (the subtitle is animated, so it stays out of this layer) */
    draw_text(g, st->title, g_title_font, L->text_x, L->block_top, L->title_w,
              ARGB_OF(255, p->text_r, p->text_g, p->text_b));
}

/* -------------------------------------------------------- dynamic material */

static void apply_dynamic_material(RgSurface *s, const RgRenderState *st,
                                   const Layout *L)
{
    int style = style_index(st->glass_style);
    float press = rg_clampf(st->press, 0.0f, 1.0f);
    float hover = rg_clampf(st->hover_t, 0.0f, 1.0f);
    float phase = rg_clampf(st->light_phase, 0.0f, 1.0f);
    const BYTE *mask;
    ARGB primary, secondary;
    int ar, ag, ab, sr, sg, sb;
    float angle, c1x, c1y, c2x, c2y, rx, ry, energy;
    int x0, y0, x1, y1, px, py;

    if (style != 1 && press <= 0.001f && hover <= 0.01f)
        return;
    mask = card_mask(L->cw, L->ch, L->radius);
    if (!mask)
        return;

    primary = accent_from(st->dark, st->accent, 255);
    secondary = accent_to(st->dark, st->accent, 255);
    ar = (primary >> 16) & 0xFF;
    ag = (primary >> 8) & 0xFF;
    ab = primary & 0xFF;
    sr = (secondary >> 16) & 0xFF;
    sg = (secondary >> 8) & 0xFF;
    sb = secondary & 0xFF;

    angle = phase * 6.28318530718f;
    c1x = (float)L->cw * (0.50f + 0.30f * cosf(angle));
    c1y = (float)L->ch * (0.32f + 0.15f * sinf(angle));
    c2x = (float)L->cw * (0.50f - 0.28f * cosf(angle));
    c2y = (float)L->ch * (0.68f - 0.13f * sinf(angle));
    rx = fmaxf(1.0f, (float)L->cw * 0.56f);
    ry = fmaxf(1.0f, (float)L->ch * 0.76f);
    energy = st->backdrop && st->backdrop->px ? 1.0f : 0.78f;

    x0 = L->ox > 0 ? L->ox : 0;
    y0 = L->oy > 0 ? L->oy : 0;
    x1 = L->ox + L->cw < s->w ? L->ox + L->cw : s->w;
    y1 = L->oy + L->ch < s->h ? L->oy + L->ch : s->h;

    for (py = y0 - L->oy; py < y1 - L->oy; py++)
    {
        for (px = x0 - L->ox; px < x1 - L->ox; px++)
        {
            int gx = L->ox + px;
            int gy = L->oy + py;
            int coverage = mask[(size_t)py * L->cw + px];
            BYTE *q;
            float v1 = 0.0f, v2 = 0.0f, top_press;
            int add[3], c;

            if (!coverage)
                continue;

            /* The overlay is applied continuously over the whole glass face.
             * Excluding the icon or title bounding boxes created a visible
             * rectangular seam once the surrounding light moved. */
            if (style == 1)
            {
                float dx = ((float)px - c1x) / rx;
                float dy = ((float)py - c1y) / ry;
                float d = dx * dx + dy * dy;
                if (d < 1.0f)
                {
                    v1 = 1.0f - d;
                    v1 *= v1;
                }
                dx = ((float)px - c2x) / rx;
                dy = ((float)py - c2y) / ry;
                d = dx * dx + dy * dy;
                if (d < 1.0f)
                {
                    v2 = 1.0f - d;
                    v2 *= v2;
                }
            }

            top_press = press * (1.0f - 0.55f * (float)py / (float)L->ch);
            add[0] = (int)((ab * v1 * 0.155f + sb * v2 * 0.075f) * energy +
                           5.0f * top_press);
            add[1] = (int)((ag * v1 * 0.155f + sg * v2 * 0.075f) * energy +
                           5.0f * top_press);
            add[2] = (int)((ar * v1 * 0.155f + sr * v2 * 0.075f) * energy +
                           5.0f * top_press);

            /* A short prism sweep on entering the card: enough to make the
             * surface feel like glass under a moving light, not a decorative
             * animation that keeps running all day. */
            if (style != 1 && hover > 0.01f)
            {
                float u = (float)px / (float)L->cw * 0.42f +
                          (float)py / (float)L->ch * 0.58f;
                float d = fabsf(u - phase);
                float band = d < 0.16f ? 1.0f - d / 0.16f : 0.0f;
                float lift = band * band * hover;
                int br = (int)rg_lerp((float)((primary >> 16) & 0xFF),
                                      (float)((secondary >> 16) & 0xFF), u);
                int bg = (int)rg_lerp((float)((primary >> 8) & 0xFF),
                                      (float)((secondary >> 8) & 0xFF), u);
                int bb = (int)rg_lerp((float)(primary & 0xFF),
                                      (float)(secondary & 0xFF), u);

                add[0] += (int)(bb * 0.060f * lift) + (int)(3.0f * lift);
                add[1] += (int)(bg * 0.060f * lift) + (int)(3.0f * lift);
                add[2] += (int)(br * 0.060f * lift) + (int)(3.0f * lift);
            }

            q = (BYTE *)s->bits + ((size_t)gy * s->w + gx) * 4;
            for (c = 0; c < 3; c++)
            {
                int delta = add[c] * coverage * q[3] / (255 * 255);
                int value;
                if (delta > 40) delta = 40;
                value = q[c] + delta;
                if (value > q[3]) value = q[3];
                q[c] = (BYTE)(value > 255 ? 255 : value);
            }
        }
    }
}

static void draw_press_rim(GpGraphics *g, const RgRenderState *st, const Layout *L)
{
    float press = rg_clampf(st->press, 0.0f, 1.0f);
    GpPath *path;
    GpLineGradient *brush;
    GpPen *pen = NULL;

    if (press <= 0.001f)
        return;
    path = make_squircle((float)L->ox + 1.0f, (float)L->oy + 1.0f,
                         (float)L->cw - 2.0f, (float)L->ch - 2.0f,
                         fmaxf(0.0f, (float)L->radius - 1.0f));
    if (!path)
        return;
    brush = accent_gradient((float)L->ox, (float)L->oy,
                            (float)L->cw, (float)L->ch,
                            accent_from(st->dark, st->accent,
                                        (int)(44.0f * press)),
                            accent_to(st->dark, st->accent,
                                      (int)(44.0f * press)));
    if (brush && GdipCreatePen2((GpBrush *)brush,
                                fmaxf(1.0f, 1.1f * g_scale),
                                UnitPixel, &pen) == Ok)
    {
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    if (brush)
        GdipDeleteBrush((GpBrush *)brush);
    GdipDeletePath(path);
}

static void draw_menu_affordance(GpGraphics *g, const RgRenderState *st,
                                 const Layout *L, const Palette *p)
{
    GpSolidFill *brush = NULL;
    float press = rg_clampf(st->press, 0.0f, 1.0f);
    float d = fmaxf(2.0f, 2.3f * g_scale);
    float gap = 5.0f * g_scale;
    float cx = (float)L->menu_x + (float)L->menu_w * 0.5f;
    float cy = L->block_top + g_title_h * 0.50f - d * 0.5f;
    int alpha = rg_clampi((st->dark ? 200 : 176) +
                          (st->hover_btn == 3 ? 40 : 0) +
                          (int)(15.0f * press), 0, 240);
    int i;

    if (st->hover_btn == 3)
    {
        GpPath *hp = make_squircle((float)L->menu_x - 2.0f, (float)L->menu_y,
                                   (float)L->menu_w + 4.0f, (float)L->menu_h,
                                   (float)L->menu_h * 0.5f);
        GpLineGradient *hf = accent_gradient(
            (float)L->menu_x, (float)L->menu_y,
            (float)L->menu_w, (float)L->menu_h,
            accent_from(st->dark, st->accent, st->dark ? 36 : 28),
            accent_to(st->dark, st->accent, st->dark ? 24 : 18));
        if (hp && hf)
        {
            GdipFillPath(g, (GpBrush *)hf, hp);
        }
        if (hf)
            GdipDeleteBrush((GpBrush *)hf);
        if (hp)
            GdipDeletePath(hp);
    }

    if (GdipCreateSolidFill(ARGB_OF(alpha, p->dim_r, p->dim_g, p->dim_b),
                            &brush) != Ok)
        return;
    for (i = -1; i <= 1; i++)
        GdipFillEllipse(g, (GpBrush *)brush, cx + i * gap - d * 0.5f, cy, d, d);
    GdipDeleteBrush((GpBrush *)brush);
}

/* ------------------------------------------------------------- public API */

void render_card(RgSurface *s, const RgRenderState *st)
{
    const Palette *p;
    GpBitmap *canvas = NULL;
    GpGraphics *g = NULL;
    Layout L;
    ContentKey key;
    size_t bytes;

    if (!s || !s->dc || !s->bits || !st)
        return;
    g_unblend_body = st->unblend_body ? 1 : 0;
    p = st->dark ? &PAL_DARK : &PAL_LIGHT;

    compute_layout(st, &L);

    memset(&key, 0, sizeof(key));
    key.w = s->w;
    key.h = s->h;
    key.dark = st->dark ? 1 : 0;
    key.accent = accent_index(st->accent);
    key.icon_style = st->icon_style ? 1 : 0;
    key.epoch = g_render_epoch;

    bytes = (size_t)s->w * s->h * 4;

    if (!g_skey_valid || !g_static || g_static_w != s->w || g_static_h != s->h ||
        memcmp(&key, &g_skey, sizeof(key)) != 0)
    {
        BYTE *nb = (BYTE *)realloc(g_static, bytes);
        if (!nb)
            return;
        g_static = nb;
        g_static_w = s->w;
        g_static_h = s->h;

        if (GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4,
                                      PixelFormat32bppPARGB, g_static, &canvas) == Ok)
        {
            GdipGetImageGraphicsContext(canvas, &g);
            GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
            GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
            GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
            GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
            GdipSetCompositingMode(g, CompositingModeSourceOver);
            GdipGraphicsClear(g, 0x00000000);

            build_content(st, g, &L);

            GdipDeleteGraphics(g);
            GdipDisposeImage(canvas);
        }

        g_skey = key;
        g_skey_valid = TRUE;
    }

    /* The glass body follows the backdrop and is cheap relative to GDI+ text,
     * so it is painted per frame; the icon/title layer is blitted 1:1 on top. */
    memset(s->bits, 0, bytes);

    if (GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4, PixelFormat32bppPARGB,
                                  s->bits, &canvas) != Ok)
        return;
    GdipGetImageGraphicsContext(canvas, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetCompositingMode(g, CompositingModeSourceOver);

    paint_glass(g, st->backdrop, st->rim, L.ox, L.oy, L.cw, L.ch, L.radius,
                st->opacity, st->material, st->drag_hover, p, st->dark,
                st->glass_style, st->accent, st->refraction, 0.0f, st->capture_body);

    {
        GpBitmap *content = NULL;

        if (g_static &&
            GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4,
                                      PixelFormat32bppPARGB, g_static,
                                      &content) == Ok)
        {
            GdipSetInterpolationMode(g, InterpolationModeNearestNeighbor);
            GdipDrawImageRectI(g, content, 0, 0, s->w, s->h);
            GdipDisposeImage(content);
        }
    }

    GdipDeleteGraphics(g);
    GdipDisposeImage(canvas);

    apply_dynamic_material(s, st, &L);

    if (GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4, PixelFormat32bppPARGB,
                                  s->bits, &canvas) != Ok)
        return;
    GdipGetImageGraphicsContext(canvas, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetCompositingMode(g, CompositingModeSourceOver);

    /* subtitle (fades out as the pills come in) */
    if (L.sub_a > 0.004f)
    {
        wchar_t sub[128];

        wcsncpy_s(sub, RG_COUNT(sub),
                  st->subtitle ? st->subtitle : L"", _TRUNCATE);
        ellipsize_text(g, sub, g_sub_font, L.content_w);
        draw_text(g, sub, g_sub_font, L.text_x,
                  L.block_top + g_title_h + L.sub_gap, L.content_w,
                  ARGB_OF((int)(255.0f * L.sub_a), p->dim_r, p->dim_g, p->dim_b));
    }

    /* Permanent title-row menu affordance. */
    g_hit_menu.left = (LONG)L.menu_x;
    g_hit_menu.top = (LONG)L.menu_y;
    g_hit_menu.right = (LONG)(L.menu_x + L.menu_w - 1);
    g_hit_menu.bottom = (LONG)(L.menu_y + L.menu_h - 1);
    draw_menu_affordance(g, st, &L, p);

    /* action pills: hit geometry follows the painted (dy-offset) row */
    g_hit_open.left = L.bx0;
    g_hit_open.top = (LONG)lroundf(L.row_top + (float)L.dy);
    g_hit_open.right = L.bx0 + L.bw - 1;
    g_hit_open.bottom = g_hit_open.top + PILL_H - 1;

    g_hit_empty.left = L.bx0 + L.bw + PILL_GAP;
    g_hit_empty.top = g_hit_open.top;
    g_hit_empty.right = g_hit_empty.left + L.bw - 1;
    g_hit_empty.bottom = g_hit_open.bottom;

    draw_press_rim(g, st, &L);

    if (L.pill_a > 0.004f)
    {
        static const RgStr labels[2] = { S_BTN_OPEN, S_BTN_EMPTY };
        int a_scale = (int)(255.0f * L.pill_a);
        int bx = L.bx0;
        int i;

        for (i = 0; i < 2; i++)
        {
            float lit = rg_clampf(st->btn_t[i], 0.0f, 1.0f);
            GpPath *pp = make_squircle((float)bx, L.row_top + (float)L.dy,
                                       (float)L.bw, (float)PILL_H, (float)PILL_R);
            if (pp)
            {
                GpSolidFill *fill = NULL;
                ARGB bg = blend_argb(p->pill_bg, p->pill_hover_bg, lit);
                ARGB bg2 = ARGB_OF(((bg >> 24) & 0xFF) * a_scale / 255,
                                   (bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);

                if (GdipCreateSolidFill(bg2, &fill) == Ok)
                {
                    GdipFillPath(g, (GpBrush *)fill, pp);
                    GdipDeleteBrush((GpBrush *)fill);
                }

                if (lit > 0.004f)
                {
                    GpLineGradient *hb = accent_gradient(
                        (float)bx, L.row_top + (float)L.dy,
                        (float)L.bw, (float)PILL_H,
                        accent_from(st->dark, st->accent,
                                    (int)(44.0f * lit * L.pill_a)),
                        accent_to(st->dark, st->accent,
                                  (int)(30.0f * lit * L.pill_a)));
                    GpLineGradient *bb = accent_gradient(
                        (float)bx, L.row_top + (float)L.dy,
                        (float)L.bw, (float)PILL_H,
                        accent_from(st->dark, st->accent,
                                    (int)((st->dark ? 170.0f : 205.0f) *
                                          lit * L.pill_a)),
                        accent_to(st->dark, st->accent,
                                  (int)((st->dark ? 140.0f : 190.0f) *
                                        lit * L.pill_a)));
                    GpPen *bp = NULL;

                    if (hb)
                    {
                        GdipFillPath(g, (GpBrush *)hb, pp);
                        GdipDeleteBrush((GpBrush *)hb);
                    }
                    if (bb && GdipCreatePen2((GpBrush *)bb, 1.0f,
                                             UnitPixel, &bp) == Ok)
                    {
                        GdipDrawPath(g, bp, pp);
                        GdipDeletePen(bp);
                    }
                    if (bb)
                        GdipDeleteBrush((GpBrush *)bb);
                }
                GdipDeletePath(pp);
            }

            {
                float tw = 0.0f;
                const wchar_t *label = T(labels[i]);
                measure_text(g, label, g_sub_font, &tw, NULL);
                draw_text(g, label, g_sub_font,
                          (float)bx + ((float)L.bw - tw) * 0.5f,
                          L.row_top + (float)L.dy + ((float)PILL_H - g_sub_h) * 0.5f,
                          (float)L.bw,
                          ARGB_OF((int)(255.0f * L.pill_a),
                                  (p->pill_text >> 16) & 0xFF,
                                  (p->pill_text >> 8) & 0xFF,
                                  p->pill_text & 0xFF));
            }

            bx += L.bw + PILL_GAP;
        }
    }

    /* ripple after a successful recycle */
    if (st->has_fx)
    {
        float t = rg_clampf(st->fx_t, 0.0f, 1.0f);
        float cx = (float)(ICON_X + ICON / 2);
        float cy = (float)(ICON_Y + ICON / 2);
        float dia = (28.0f + 84.0f * t) * g_scale;
        int alpha = (int)(215.0f * powf(1.0f - t, 1.4f));
        float th = fmaxf(1.0f, lroundf((3.0f - 2.0f * t) * g_scale));

        if (alpha > 4)
        {
            GpLineGradient *rb = accent_gradient(
                cx - dia * 0.5f, cy - dia * 0.5f, dia, dia,
                accent_from(st->dark, st->accent, alpha),
                accent_to(st->dark, st->accent, alpha));
            GpPen *rp = NULL;
            if (rb && GdipCreatePen2((GpBrush *)rb, th, UnitPixel, &rp) == Ok)
            {
                GdipDrawEllipse(g, rp, cx - dia * 0.5f, cy - dia * 0.5f, dia, dia);
                GdipDeletePen(rp);
            }
            if (rb)
                GdipDeleteBrush((GpBrush *)rb);
        }
    }

    GdipDeleteGraphics(g);
    GdipDisposeImage(canvas);

    /* Text and the icon chip are drawn opaque on top of the body, which makes
     * them unrecoverable for self_unblend(): the compositor equation has no
     * solution at a = 1, and those pixels would survive in the backdrop as
     * leftovers of the card.  Scaling the whole surface once - in premultiplied
     * form, colour and alpha together, so it stays a valid PARGB bitmap - caps
     * every pixel at the same 0.906 the body already uses.  The card looks
     * identical; the inversion simply has signal everywhere. */
    if (g_unblend_body)
    {
        size_t pxi, pxn = (size_t)s->w * (size_t)s->h;
        BYTE *px = (BYTE *)s->bits;

        for (pxi = 0; pxi < pxn; pxi++, px += 4)
        {
            if (px[3] == 0)
                continue;
            px[0] = (BYTE)((px[0] * 232) >> 8);
            px[1] = (BYTE)((px[1] * 232) >> 8);
            px[2] = (BYTE)((px[2] * 232) >> 8);
            px[3] = (BYTE)((px[3] * 232) >> 8);
        }
    }
}
