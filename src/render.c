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
    34, 302, 118, 28,
    22, 16, 64, 18,
    32, 8, 10, 96,
    12, 6
};

/* Layout, rebuilt by render_set_scale(). */
static float g_scale = 1.0f;
static int   PAD, CARD_W, CARD_H, RADIUS;
static int   TITLE_FS, SUB_FS, ICON, ICON_X, ICON_Y;
static int   PILL_H, PILL_GAP, PILL_R, PILL_MAX_W;
static int   SHADOW_BLUR, SHADOW_DY, SHADOW_PAD;
static int   WINDOW_W, WINDOW_H;

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
    int rim_th, rim_top_a, rim_bot_a;
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

static const Palette PAL_LIGHT = {
    252, 252, 255,
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
    2, 122, 40,
    /* Light glass used to bottom out at 74% opacity and 50% tint, which left the
     * opacity and material sliders almost no room to move.  Same span as the
     * dark palette now. */
    0.22f, 0.72f,
    0.30f, 0.70f,
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
    2, 122, 34,
    0.18f, 0.74f,
    0.00f, 1.00f,
    130,
    44, 0, 26,
    126, 186, 252
};

/* --------------------------------------------------------------- GDI+ glue */

static ULONG_PTR g_gdiplus_token = 0;
static HINSTANCE  g_inst = NULL;
static GpFont    *g_title_font = NULL;
static GpFont    *g_sub_font = NULL;
static GpFontFamily *g_family = NULL;
static GpStringFormat *g_fmt = NULL;
static GpImage   *g_icon = NULL;
static IStream   *g_icon_stream = NULL;

/* Size the icon is baked down to: enough for a 2x-DPI card, small enough that
 * drawing it is a plain bitmap blit. */
#define ICON_BAKE 128

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
static int       g_shadow_w = 0, g_shadow_h = 0, g_shadow_a = -1;

static BYTE *g_body = NULL;
static size_t g_body_cap = 0;
static unsigned g_body_gen;        /* bumped whenever g_body is reallocated */
static void   *g_body_bmp;         /* cached GDI+ wrapper around g_body */
static unsigned g_body_bmp_gen;
static int     g_body_w, g_body_h;

/* Cached static layer (shadow + glass body + rim + icon + title).  Rebuilt only
 * when something that feeds it changes; every other frame is a memcpy plus the
 * animated text and pills. */
static BYTE *g_static = NULL;
static int   g_static_w = 0, g_static_h = 0;

typedef struct {
    int w, h, dark, drag, op, mat;
    int bx, by, bw, bh, blur10;
    unsigned bdgen;
} StaticKey;

static StaticKey g_skey;
static BOOL      g_skey_valid;

RECT g_hit_open  = { 0, 0, 0, 0 };
RECT g_hit_empty = { 0, 0, 0, 0 };

#define SQ_N     3.0f   /* superellipse exponent — matches squircle.N */
#define SQ_STEPS 96     /* points per corner quarter */

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
    glass_set_scale(s);

    PAD        = scale_k(BASE.pad, 16);
    CARD_W     = scale_k(BASE.card_w, 200);
    CARD_H     = scale_k(BASE.card_h, 78);
    RADIUS     = scale_k(BASE.radius, 10);
    TITLE_FS   = scale_k(BASE.title_fs, 11);
    SUB_FS     = scale_k(BASE.sub_fs, 9);
    ICON       = scale_k(BASE.icon, 34);
    ICON_X     = PAD + scale_k(BASE.icon_left, 10);
    ICON_Y     = PAD + (CARD_H - ICON) / 2;
    PILL_H     = scale_k(BASE.pill_h, 18);
    PILL_GAP   = scale_k(BASE.pill_gap, 4);
    PILL_R     = scale_k(BASE.pill_r, 5);
    PILL_MAX_W = scale_k(BASE.pill_max_w, 52);
    SHADOW_BLUR = scale_k(BASE.shadow_blur, 4);
    SHADOW_DY   = scale_k(BASE.shadow_dy, 2);
    SHADOW_PAD  = SHADOW_BLUR * 2 + 4;
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
    float r = (float)radius;
    int y, x;

    if (r * 2.0f > (float)w) r = (float)w * 0.5f;
    if (r * 2.0f > (float)h) r = (float)h * 0.5f;
    if (r < 0.0f) r = 0.0f;

    for (y = 0; y < h; y++)
    {
        float dy = (float)y + 0.5f;
        float v = -1.0f;
        float edge = 0.0f;
        float right;
        BYTE *row = dst + (size_t)y * w;

        if (r > 0.0f)
        {
            if (dy < r)
                v = (r - dy) / r;
            else if (dy > (float)h - r)
                v = (dy - ((float)h - r)) / r;
        }

        if (v > 0.0f)
        {
            if (v > 1.0f) v = 1.0f;
            edge = r - r * powf(1.0f - powf(v, SQ_N), 1.0f / SQ_N);
            if (edge < 0.0f) edge = 0.0f;
        }

        right = (float)w - edge;

        for (x = 0; x < w; x++)
        {
            float x0 = (float)x;
            float x1 = x0 + 1.0f;
            float cov = 0.0f;

            if (x1 > edge && x0 < right)
            {
                float a = (x1 < right) ? x1 : right;
                float b = (x0 > edge) ? x0 : edge;
                cov = a - b;
                if (cov > 1.0f) cov = 1.0f;
            }
            row[x] = (BYTE)(cov * 255.0f + 0.5f);
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

    if (g_shadow && g_shadow_w == w && g_shadow_h == h && g_shadow_a == alpha)
        return g_shadow;

    if (g_shadow)
    {
        GdipDisposeImage(g_shadow);
        g_shadow = NULL;
    }
    g_shadow_w = w;
    g_shadow_h = h;
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
    if (g_family)     { GdipDeleteFontFamily(g_family); g_family = NULL; }
}

static void build_fonts(void)
{
    static const wchar_t *cjk_names[] = { L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei" };
    static const wchar_t *lat_names[] = { L"Segoe UI", L"Tahoma" };
    const wchar_t **names = i18n_is_cjk() ? cjk_names : lat_names;
    int count = i18n_is_cjk() ? RG_COUNT(cjk_names) : RG_COUNT(lat_names);
    int i;

    drop_fonts();

    for (i = 0; i < count && !g_family; i++)
    {
        GpFontFamily *fam = NULL;
        if (GdipCreateFontFamilyFromName(names[i], NULL, &fam) == Ok && fam)
            g_family = fam;
    }
    if (!g_family)
        GdipGetGenericFontFamilySansSerif(&g_family);

    if (g_family)
    {
        GdipCreateFont(g_family, (REAL)TITLE_FS, FontStyleBold, UnitPixel, &g_title_font);
        GdipCreateFont(g_family, (REAL)SUB_FS, FontStyleRegular, UnitPixel, &g_sub_font);
    }

    measure_font_heights();
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

    if (!s || !*s || !font)
        return;

    rect.X = x;
    rect.Y = y;
    rect.Width = w;
    rect.Height = 4000.0f;

    if (GdipCreateSolidFill(color, &brush) != Ok)
        return;

    GdipDrawString(g, s, -1, font, &rect, g_fmt, (GpBrush *)brush);
    GdipDeleteBrush((GpBrush *)brush);
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
    g_shadow_w = g_shadow_h = 0;
    g_shadow_a = -1;

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
    int   cw, ch, ox, oy;
    float text_x, content_w;
    float sub_gap, row_gap;
    float block_top, row_top;
    float sub_a, pill_a;
    int   dy, bw, total_w, bx0;
    float ht;
} Layout;

static void compute_layout(const RgRenderState *st, Layout *L)
{
    float idle_h;

    L->cw = CARD_W;
    L->ch = CARD_H;
    L->ox = PAD;
    L->oy = PAD;

    L->ht = st->hover ? rg_ease_out(rg_clampf(st->hover_t, 0.0f, 1.0f)) : 0.0f;

    L->text_x = (float)(ICON_X + ICON) + fmaxf(14.0f, lroundf(18.0f * g_scale));
    L->content_w = fmaxf(60.0f,
                         (float)(PAD + CARD_W) - fmaxf(12.0f, lroundf(20.0f * g_scale))
                         - L->text_x);

    L->sub_gap = fmaxf(3.0f, lroundf(5.0f * g_scale));
    L->row_gap = fmaxf(5.0f, lroundf(8.0f * g_scale));

    idle_h = g_title_h + L->sub_gap + g_sub_h;

    /* Anchored on the resting layout.  Hovering swaps the second row in place;
     * re-centring the block for the taller pill row moved the title up by ~10
     * px, which read as the text jumping under the pointer. */
    L->block_top = (float)PAD + lroundf((float)(CARD_H - idle_h) * 0.5f);
    L->row_top = L->block_top + g_title_h +
                 lroundf(L->sub_gap + (L->row_gap - L->sub_gap) * L->ht);

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
}

/* ------------------------------------------------------------ glass panel */

/* Shadow + blurred backdrop + tint + sheen + rim + border, for an arbitrary
 * box.  Both the card and the context menu go through this, which is why the
 * menu reads as the same material as the widget. */
/* --------------------------------------------------------------- rim layer
 *
 * The light catch along the card edge: a 1-2 px stroke hugging the silhouette,
 * bright along the top and gone by the bottom - light falling from above.
 *
 * It is rasterised once into a cached layer for three reasons: GDI+'s preset
 * blend is a no-op on a two-colour line brush, so a gradient brush cannot
 * express the falloff; a centred stroke at the silhouette spills half a pixel
 * *outside* the card, which reads as a pale hairline over the wallpaper; and
 * the layer never changes while the card is being dragged.
 */
static GpBitmap *g_rim;
static int g_rim_w, g_rim_h, g_rim_r, g_rim_a, g_rim_brd;

/* A ~1.4 px stroke sitting on the silhouette (half of it spills outside, which
 * is what gives the outer bloom), alpha 92 along the top edge easing down the
 * sides.  It reaches zero by 92% of the height: any residual alpha at the very
 * bottom row reads as a bright hairline lying across the glass, which is the
 * "gradient boundary" that kept showing up on dark wallpapers.  Positions are
 * fractions of the card height. */
static const float RG_RIM_T[6] = { 0.00f, 0.18f, 0.39f, 0.60f, 0.80f, 0.92f };
static const float RG_RIM_K[6] = { 1.00f, 0.90f, 0.58f, 0.51f, 0.30f, 0.00f };
#define RG_RIM_W 1.4f

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

static GpBitmap *rim_layer(int w, int h, int radius, int a0, int th, ARGB border)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GpPath *path = NULL;
    GpPen *pen = NULL;
    BitmapData bd;
    int x, y;

    (void)th;

    if (g_rim && g_rim_w == w && g_rim_h == h && g_rim_r == radius &&
        g_rim_a == a0 && g_rim_brd == (int)border)
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

    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB, NULL, &bmp) != Ok)
        return NULL;

    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipGraphicsClear(g, 0);

    /* Centred on the silhouette: half a pixel of the stroke lands outside the
     * card, which is exactly the bloom the reference has. */
    path = make_squircle(0.5f, 0.5f, (float)w - 1.0f, (float)h - 1.0f, (float)radius);
    if (path)
    {
        if (GdipCreatePen1(ARGB_OF(a0, 255, 255, 255), RG_RIM_W, UnitPixel, &pen) == Ok)
        {
            GdipDrawPath(g, pen, path);
            GdipDeletePen(pen);
            pen = NULL;
        }
        if (((border >> 24) & 0xFF) > 0 &&
            GdipCreatePen1(border, 1.0f, UnitPixel, &pen) == Ok)
        {
            GdipDrawPath(g, pen, path);
            GdipDeletePen(pen);
            pen = NULL;
        }
        GdipDeletePath(path);
    }
    GdipDeleteGraphics(g);

    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(bmp, NULL, ImageLockModeRead | ImageLockModeWrite,
                           PixelFormat32bppPARGB, &bd) == Ok)
    {
        for (y = 0; y < h; y++)
        {
            int fi = (int)(rim_gain(h > 1 ? (float)y / (float)(h - 1) : 0.0f) * 256.0f);
            BYTE *row = (BYTE *)bd.Scan0 + (size_t)y * bd.Stride;

            if (fi >= 256)
                continue;
            for (x = 0; x < w; x++)
            {
                BYTE *q = row + x * 4;

                if (!q[3])
                    continue;
                q[0] = (BYTE)((q[0] * fi) >> 8);
                q[1] = (BYTE)((q[1] * fi) >> 8);
                q[2] = (BYTE)((q[2] * fi) >> 8);
                q[3] = (BYTE)((q[3] * fi) >> 8);
            }
        }
        GdipBitmapUnlockBits(bmp, &bd);
    }

    g_rim = bmp;
    return g_rim;
}

static void paint_glass(GpGraphics *g, const RgBitmap *bd,
                        int x, int y, int w, int h, int radius,
                        float opacity, float material, int drag_hover,
                        const Palette *p)
{
    float fac = rg_clampf(p->opacity_floor + p->opacity_span * opacity,
                          0.15f, 1.0f);
    const BYTE *mask;

    /* 1. shadow */
    {
        GpBitmap *sh = shadow_sprite(w, h, radius,
                                     rg_clampi(p->shadow_alpha, 0, 255));
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
        int tint_a = (int)(rg_clampf(p->tint_floor + p->tint_span * material,
                                     0.0f, 1.0f) * 256.0f + 0.5f);
        int fac8   = (int)(fac * 256.0f + 0.5f);
        int drag8  = 0;
        int drag_r = 0, drag_g = 0, drag_b = 0;
        const BYTE *src = NULL;
        size_t src_stride = 0;
        int px, py;

        if (drag_hover)
        {
            drag8 = (int)(((float)((p->drag_wash >> 24) & 0xFF) / 255.0f *
                           rg_clampf(opacity, 0.0f, 1.0f)) * 256.0f + 0.5f);
            drag_r = (p->drag_wash >> 16) & 0xFF;
            drag_g = (p->drag_wash >> 8) & 0xFF;
            drag_b = p->drag_wash & 0xFF;
        }

        if (bd && bd->px && bd->w >= w && bd->h >= h)
        {
            src = bd->px;
            src_stride = (size_t)bd->w * 4;
        }

        for (py = 0; py < h; py++)
        {
            int sh = sheen_alpha(py, h, p->sheen_top, p->sheen_top / 3,
                                 p->bottom_shade);
            BYTE *dst = g_body + (size_t)py * w * 4;
            const BYTE *sp = src ? src + (size_t)py * src_stride : NULL;
            const BYTE *mp = mask + (size_t)py * w;

            for (px = 0; px < w; px++, dst += 4, mp++)
            {
                int r, g, b, a;

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

                /* tint towards the material colour */
                r += ((p->tint_r - r) * tint_a) >> 8;
                g += ((p->tint_g - g) * tint_a) >> 8;
                b += ((p->tint_b - b) * tint_a) >> 8;

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

                /* wash while dragging */
                if (drag8)
                {
                    r += ((drag_r - r) * drag8) >> 8;
                    g += ((drag_g - g) * drag8) >> 8;
                    b += ((drag_b - b) * drag8) >> 8;
                }

                a = (*mp * fac8) >> 8;
                if (a <= 0)
                {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                if (a > 255)
                    a = 255;

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
        GpBitmap *rl = rim_layer(w, h, radius, (int)(p->rim_top_a * fac),
                                p->rim_th, p->border);
        if (rl)
            GdipDrawImageRectI(g, rl, x, y, w, h);
    }
}

void render_glass_panel(RgSurface *s, const RgBitmap *backdrop,
                        int x, int y, int w, int h, int radius,
                        float opacity, float material, int dark)
{
    const Palette *p = dark ? &PAL_DARK : &PAL_LIGHT;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;

    if (!s || !s->bits || w <= 2 || h <= 2)
        return;

    if (GdipCreateBitmapFromScan0(s->w, s->h, s->w * 4, PixelFormat32bppPARGB,
                                  s->bits, &bmp) != Ok)
        return;

    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetCompositingMode(g, CompositingModeSourceOver);

    paint_glass(g, backdrop, x, y, w, h, radius, opacity, material, 0, p);

    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);
}

void render_theme(int dark, RgTheme *out)
{
    const Palette *p = dark ? &PAL_DARK : &PAL_LIGHT;

    if (!out)
        return;

    out->text        = ARGB_OF(255, p->text_r, p->text_g, p->text_b);
    out->dim         = ARGB_OF(255, p->dim_r, p->dim_g, p->dim_b);
    out->accent      = ARGB_OF(255, p->accent_r, p->accent_g, p->accent_b);
    out->accent_soft = ARGB_OF(38, p->accent_r, p->accent_g, p->accent_b);
    out->border      = p->border;
    out->hover_bg    = dark ? ARGB_OF(30, 255, 255, 255) : ARGB_OF(20, 0, 0, 0);
    out->track       = dark ? ARGB_OF(46, 255, 255, 255) : ARGB_OF(28, 0, 0, 0);
    out->pill_bg     = p->pill_bg;
    out->pill_text   = p->pill_text;
    out->switch_off  = dark ? ARGB_OF(72, 255, 255, 255) : ARGB_OF(40, 0, 0, 0);
    out->switch_on   = ARGB_OF(255, 10, 132, 255);
}

/* Everything that does not move: shadow, glass body, rim, border, icon, title. */
static void build_static(const RgRenderState *st, GpGraphics *g, const Layout *L)
{
    const Palette *p = st->dark ? &PAL_DARK : &PAL_LIGHT;

    paint_glass(g, st->backdrop, L->ox, L->oy, L->cw, L->ch, RADIUS,
                st->opacity, st->material, st->drag_hover, p);

    /* 4. icon */
    if (g_icon)
        GdipDrawImageRectI(g, g_icon, ICON_X, ICON_Y, ICON, ICON);

    /* 5. title (the subtitle is animated, so it stays out of this layer) */
    draw_text(g, st->title, g_title_font, L->text_x, L->block_top, L->content_w,
              ARGB_OF(255, p->text_r, p->text_g, p->text_b));
}

/* ------------------------------------------------------------- public API */

void render_card(RgSurface *s, const RgRenderState *st)
{
    const Palette *p = st->dark ? &PAL_DARK : &PAL_LIGHT;
    GpBitmap *canvas = NULL;
    GpGraphics *g = NULL;
    Layout L;
    StaticKey key;
    size_t bytes;

    if (!s || !s->dc || !s->bits || !st)
        return;

    compute_layout(st, &L);

    memset(&key, 0, sizeof(key));
    key.w = s->w;
    key.h = s->h;
    key.dark = st->dark ? 1 : 0;
    key.drag = st->drag_hover ? 1 : 0;
    key.op = (int)(st->opacity * 255.0f);
    key.mat = (int)(st->material * 255.0f);
    key.bx = st->backdrop_x;
    key.by = st->backdrop_y;
    key.bw = st->backdrop ? st->backdrop->w : 0;
    key.bh = st->backdrop ? st->backdrop->h : 0;
    key.blur10 = (int)(st->blur * 10.0f);
    key.bdgen = st->backdrop_gen;

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

            build_static(st, g, &L);

            GdipDeleteGraphics(g);
            GdipDisposeImage(canvas);
        }

        g_skey = key;
        g_skey_valid = TRUE;
    }

    memcpy(s->bits, g_static, bytes);

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
        draw_text(g, st->subtitle, g_sub_font, L.text_x,
                  L.block_top + g_title_h + L.sub_gap, L.content_w,
                  ARGB_OF((int)(255.0f * L.sub_a), p->dim_r, p->dim_g, p->dim_b));
    }

    /* action pills */
    g_hit_open.left = L.bx0;
    g_hit_open.top = (LONG)lroundf(L.row_top);
    g_hit_open.right = L.bx0 + L.bw - 1;
    g_hit_open.bottom = (LONG)lroundf(L.row_top) + PILL_H - 1;

    g_hit_empty.left = L.bx0 + L.bw + PILL_GAP;
    g_hit_empty.top = g_hit_open.top;
    g_hit_empty.right = g_hit_empty.left + L.bw - 1;
    g_hit_empty.bottom = g_hit_open.bottom;

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
                    GpPen *bp = NULL;
                    ARGB bc = p->pill_hover_border;
                    if (GdipCreatePen1(ARGB_OF((int)(((bc >> 24) & 0xFF) * lit) *
                                               a_scale / 255,
                                               (bc >> 16) & 0xFF, (bc >> 8) & 0xFF,
                                               bc & 0xFF),
                                       1.0f, UnitPixel, &bp) == Ok)
                    {
                        GdipDrawPath(g, bp, pp);
                        GdipDeletePen(bp);
                    }
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
            GpPen *rp = NULL;
            ARGB rc = p->drag_ring;
            if (GdipCreatePen1(ARGB_OF(alpha, (rc >> 16) & 0xFF,
                                       (rc >> 8) & 0xFF, rc & 0xFF),
                               th, UnitPixel, &rp) == Ok)
            {
                GdipDrawEllipse(g, rp, cx - dia * 0.5f, cy - dia * 0.5f, dia, dia);
                GdipDeletePen(rp);
            }
        }
    }

    GdipDeleteGraphics(g);
    GdipDisposeImage(canvas);
}
