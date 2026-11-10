/* Self-drawn context menu.
 *
 * The widget window is WS_EX_NOACTIVATE and never becomes foreground, which
 * makes TrackPopupMenu unreliable (the menu can be dismissed instantly or not
 * appear at all).  This is our own layered popup instead: the same glass
 * material as the card (shadow, blurred wallpaper, tint, sheen, rim), Lucide
 * icons, switches, segmented pickers, live sliders and an inline confirmation
 * for emptying the bin.
 *
 * Every state change is animated with an ease-out curve: the panel fades and
 * slides in, pages cross-slide, row highlights fade, switch knobs slide and
 * segment thumbs glide.  A short timer drives the frames while anything moves
 * and stops as soon as everything has settled. */
#include "menu.h"
#include "i18n.h"
#include "render.h"
#include "icon.h"
#include "shell.h"

#define MENU_CLASS L"RecycleGlassMenuC"

enum { ROW_SEP = 0, ROW_CMD, ROW_TOGGLE, ROW_PAGE, ROW_BACK, ROW_SLIDER,
       ROW_SEGMENT, ROW_CONFIRM, ROW_GAP };

enum { PAGE_MAIN = 0, PAGE_APPEARANCE = 1, PAGE_DELETE = 2 };

typedef struct {
    int            kind;
    int            id;      /* command id, page index, or RG_SET_* */
    const wchar_t *label;
    wchar_t        value[48];
    int            checked;
    int            enabled;
    int            icon;    /* RgIcon */
    float          v, vmin, vmax;
    int            percent; /* slider readout: 1 -> %, 0 -> px */
    int            nseg;
    const wchar_t *seg[4];
    int            seg_cmd[4];
    int            seg_cur;
} Row;

/* ------------------------------------------------------------- animation */

typedef struct {
    float     v, from, to;
    ULONGLONG t0;
    int       dur;
    BOOL      active;
} Anim;

static void anim_to(Anim *a, float to, int dur)
{
    if (a->active && fabsf(a->to - to) < 0.001f)
        return;
    if (!a->active && fabsf(a->v - to) < 0.001f)
        return;
    a->from = a->v;
    a->to = to;
    a->t0 = GetTickCount64();
    a->dur = dur;
    a->active = TRUE;
}

/* Ease-out cubic: fast start, soft landing. */
static float anim_ease(float t)
{
    float k = rg_clampf(t, 0.0f, 1.0f);
    k = 1.0f - k;
    return 1.0f - k * k * k;
}

static BOOL anim_step(Anim *a)
{
    float t;

    if (!a->active)
        return FALSE;

    t = (float)(GetTickCount64() - a->t0) / (float)(a->dur > 0 ? a->dur : 1);
    if (t >= 1.0f)
    {
        a->v = a->to;
        a->active = FALSE;
        return FALSE;
    }
    a->v = a->from + (a->to - a->from) * anim_ease(t);
    return TRUE;
}

static MenuHost  *g_host;
static HWND       g_hwnd;
static RgSurface  g_surf;
static GpFontFamily *g_family;
static GpFont    *g_font;
static GpFont    *g_font_small;
static GpStringFormat *g_fmt_l;
static GpStringFormat *g_fmt_r;
static GpStringFormat *g_fmt_c;

static Row  g_rows[24];
static int  g_nrows;
static int  g_page;
static int  g_hover = -1;

static int  g_drag_slider = -1;
static int  g_drag_slider_final;    /* row index being dragged */
static ULONGLONG g_slider_tick;
static BOOL g_shown;
static BOOL g_closing;
static ULONGLONG g_shown_at;
static int  g_win_x, g_win_y;

static RgBitmap g_bd;
static BOOL     g_bd_valid;
static BOOL     g_live;         /* blur the real desktop behind the panel */
static int      g_bd_x, g_bd_y, g_bd_q;
static RgBitmap g_bd_raw;        /* unblurred capture, taller than the panel */
static RgBitmap g_bd_blur;       /* the same pixels blurred at g_bd_raw_q */
static int      g_bd_raw_x, g_bd_raw_y;
static int      g_bd_raw_q = -1;

static Anim g_open;              /* 0 hidden .. 1 shown */
static Anim g_page_anim;         /* 0..1 page cross-slide */
static int  g_page_dir = 1;
static Anim g_hover_anim[24];
static Anim g_sw_anim[24];
static Anim g_seg_anim[24];
static Anim g_confirm_anim;
static int  g_confirm_row = -1;
static int  g_confirm_hover;    /* 0 none, 1 cancel, 2 empty */

static int  PAD, ROW_H, SEP_H, RADIUS, FS, PANEL_W, ICON_SZ, SLIDER_H,
            SEG_H, CONFIRM_EXTRA, MARGIN, ROW_R;
static RECT g_rowrect[24];

/* ------------------------------------------------------------------ layout */

static float menu_scale(void)
{
    if (g_host && g_host->query_scale)
        return g_host->query_scale();
    return (g_host && g_host->scale > 0.0f) ? g_host->scale : 1.0f;
}

static int menu_dark(void)
{
    if (g_host && g_host->query_dark)
        return g_host->query_dark();
    return g_host ? g_host->dark : 0;
}

static int sc(int base, int lo)
{
    float s = menu_scale();
    int v = (int)lround((double)base * (double)s);
    return v < lo ? lo : v;
}

static float scf(float base, float lo)
{
    float s = menu_scale();
    float v = base * s;
    return v < lo ? lo : v;
}

static void relayout(void)
{
    PAD           = sc(10, 6);
    ROW_H         = sc(44, 30);
    SEP_H         = sc(11, 7);
    RADIUS        = sc(18, 11);
    FS            = sc(16, 12);
    PANEL_W       = sc(308, 208);
    ICON_SZ       = sc(19, 14);
    SLIDER_H      = sc(60, 42);
    SEG_H         = sc(76, 52);
    CONFIRM_EXTRA = sc(62, 42);
    MARGIN        = render_shadow_pad();
    ROW_R         = sc(8, 5);
}

/* The prompt is either fully open (it owns its page) or folded away. */
static float confirm_open(void)
{
    return (g_page == PAGE_DELETE) ? 1.0f : 0.0f;
}

static int row_height(const Row *r)
{
    if (r->kind == ROW_SEP)
        return SEP_H;
    if (r->kind == ROW_GAP)
        return (int)lroundf(r->v);
    if (r->kind == ROW_SLIDER)
        return SLIDER_H;
    if (r->kind == ROW_SEGMENT)
        return SEG_H;
    if (r->kind == ROW_CONFIRM)
        return ROW_H + (int)lroundf((float)CONFIRM_EXTRA * confirm_open());
    return ROW_H;
}

static int rows_height(void)
{
    int h = PAD * 2, i;
    for (i = 0; i < g_nrows; i++)
        h += row_height(&g_rows[i]);
    return h;
}

/* -------------------------------------------------------------- row model */

static int theme_index(void)
{
    if (wcscmp(g_host->cfg->theme, L"light") == 0) return 1;
    if (wcscmp(g_host->cfg->theme, L"dark") == 0)  return 2;
    return 0;
}

static int size_index(void)
{
    if (wcscmp(g_host->cfg->size, L"small") == 0) return 0;
    if (wcscmp(g_host->cfg->size, L"large") == 0) return 2;
    return 1;
}

static Row *add(int kind, int id, const wchar_t *label, int icon)
{
    Row *r = &g_rows[g_nrows++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->id = id;
    r->label = label;
    r->icon = icon;
    r->enabled = 1;
    return r;
}

static void build_rows(void)
{
    Row *r;
    int i;

    g_nrows = 0;

    if (g_page == 0)
    {
        add(ROW_CMD, RG_CMD_OPEN, T(S_MENU_OPEN), IC_FOLDER_OPEN);

        r = add(ROW_PAGE, PAGE_DELETE, T(S_MENU_EMPTY), IC_TRASH);
        r->enabled = (g_host->count > 0);

        add(ROW_SEP, 0, NULL, -1);
        add(ROW_PAGE, 1, T(S_MENU_APPEARANCE), IC_SLIDERS);
        add(ROW_SEP, 0, NULL, -1);

        r = add(ROW_TOGGLE, RG_CMD_TOPMOST, T(S_MENU_TOPMOST), IC_PIN);
        r->checked = g_host->cfg->topmost;

        r = add(ROW_TOGGLE, RG_CMD_AUTOSTART, T(S_MENU_AUTOSTART), IC_POWER);
        r->checked = g_host->cfg->autostart;

        r = add(ROW_TOGGLE, RG_CMD_DPI, T(S_MENU_DPI), IC_MONITOR);
        r->checked = g_host->cfg->auto_dpi;

        add(ROW_SEP, 0, NULL, -1);
        add(ROW_CMD, RG_CMD_EXIT, T(S_MENU_EXIT), IC_X);
    }
    else if (g_page == PAGE_DELETE)
    {
        /* Emptying the bin gets its own screen: the prompt needs room for the
         * count, the warning and two buttons, and growing a row in place made
         * the whole panel jump while it animated. */
        add(ROW_BACK, 0, T(S_MENU_EMPTY), IC_CHEVRON_LEFT);
        add(ROW_SEP, 0, NULL, -1);

        r = add(ROW_CONFIRM, RG_CMD_EMPTY, T(S_MENU_EMPTY), IC_TRASH);
        r->enabled = (g_host->count > 0);
    }
    else
    {
        add(ROW_BACK, 0, T(S_MENU_APPEARANCE), IC_CHEVRON_LEFT);
        add(ROW_SEP, 0, NULL, -1);

        r = add(ROW_SEGMENT, 0, T(S_SET_THEME), menu_dark() ? IC_MOON : IC_SUN);
        r->nseg = 3;
        r->seg[0] = T(S_OPT_SYSTEM);
        r->seg[1] = T(S_OPT_LIGHT);
        r->seg[2] = T(S_OPT_DARK);
        r->seg_cmd[0] = RG_CMD_THEME_SYSTEM;
        r->seg_cmd[1] = RG_CMD_THEME_LIGHT;
        r->seg_cmd[2] = RG_CMD_THEME_DARK;
        r->seg_cur = theme_index();

        r = add(ROW_SEGMENT, 0, T(S_SET_SIZE), IC_MAXIMIZE);
        r->nseg = 3;
        r->seg[0] = T(S_OPT_SMALL);
        r->seg[1] = T(S_OPT_NORMAL);
        r->seg[2] = T(S_OPT_LARGE);
        r->seg_cmd[0] = RG_CMD_SIZE_SMALL;
        r->seg_cmd[1] = RG_CMD_SIZE_NORMAL;
        r->seg_cmd[2] = RG_CMD_SIZE_LARGE;
        r->seg_cur = size_index();

        r = add(ROW_SEGMENT, 0, T(S_SET_BLUR_MODE), IC_WIND);
        r->nseg = 2;
        r->seg[0] = T(S_OPT_NATIVE);
        r->seg[1] = T(S_OPT_WALLPAPER);
        r->seg_cmd[0] = RG_CMD_BLUR_SCREEN;
        r->seg_cmd[1] = RG_CMD_BLUR_WALLPAPER;
        r->seg_cur = (wcscmp(g_host->cfg->blur_mode, L"wallpaper") == 0) ? 1 : 0;

        add(ROW_SEP, 0, NULL, -1);

        r = add(ROW_SLIDER, RG_SET_OPACITY, T(S_SET_OPACITY), IC_DROPLET);
        r->vmin = 0.30f;
        r->vmax = 1.00f;
        r->v = rg_clampf(g_host->cfg->opacity, r->vmin, r->vmax);
        r->percent = 1;

        r = add(ROW_SLIDER, RG_SET_MATERIAL, T(S_SET_MATERIAL), IC_LAYERS);
        r->vmin = 0.00f;
        r->vmax = 1.00f;
        r->v = rg_clampf(g_host->cfg->material, r->vmin, r->vmax);
        r->percent = 1;

        r = add(ROW_SLIDER, RG_SET_BLUR, T(S_SET_BLUR), IC_WIND);
        r->vmin = 0.0f;     /* 0 really is no blur: the capture is used 1:1 */
        r->vmax = 48.0f;
        r->v = rg_clampf(g_host->cfg->blur_radius, r->vmin, r->vmax);
        r->percent = 0;

        add(ROW_SEP, 0, NULL, -1);
        add(ROW_CMD, RG_CMD_RESET, T(S_MENU_RESET), IC_ROTATE_CCW);
    }

    /* Seed animated values so a fresh page does not glide in from stale state. */
    for (i = 0; i < g_nrows; i++)
    {
        if (!g_hover_anim[i].active)
            g_hover_anim[i].v = (i == g_hover) ? 1.0f : 0.0f;
        if (!g_sw_anim[i].active)
            g_sw_anim[i].v = g_rows[i].checked ? 1.0f : 0.0f;
        if (!g_seg_anim[i].active)
            g_seg_anim[i].v = (float)g_rows[i].seg_cur;
    }
}

/* --------------------------------------------------------------- drawing */

static ARGB rgba(int a, int r, int g, int b)
{
    return ((ARGB)(a & 0xFF) << 24) | ((ARGB)(r & 0xFF) << 16) |
           ((ARGB)(g & 0xFF) << 8) | (ARGB)(b & 0xFF);
}

/* Multiplies a colour's alpha, used for page transitions and disabled rows. */
static ARGB fade(ARGB c, float k)
{
    int a = (int)((float)((c >> 24) & 0xFF) * rg_clampf(k, 0.0f, 1.0f));
    return (c & 0x00FFFFFF) | ((ARGB)a << 24);
}

/* Superellipse outline, same curve as the card corners. */
/* n = 2 is a true ellipse (a capsule when radius == h/2); n = 3 is the
 * superellipse the panels and pills use.  Switches must be n = 2: their knob
 * is a real circle, and a n = 3 track around it reads as "square outside,
 * round inside". */
static GpPath *round_path_n(float x, float y, float w, float h, float radius,
                            float n)
{
    const int steps = 10;
    static const float start[4] = { 180.0f, 270.0f, 0.0f, 90.0f };
    float cx[4], cy[4];
    GpPath *path = NULL;
    GpPointF pts[4 * (steps + 1)];
    int np = 0, c, i;

    if (radius * 2.0f > w) radius = w * 0.5f;
    if (radius * 2.0f > h) radius = h * 0.5f;
    if (radius < 0.0f) radius = 0.0f;
    if (n < 2.0f) n = 2.0f;

    cx[0] = x + radius;     cy[0] = y + radius;
    cx[1] = x + w - radius; cy[1] = y + radius;
    cx[2] = x + w - radius; cy[2] = y + h - radius;
    cx[3] = x + radius;     cy[3] = y + h - radius;

    for (c = 0; c < 4; c++)
    {
        for (i = 0; i <= steps; i++)
        {
            float deg = start[c] + 90.0f * (float)i / (float)steps;
            float t = deg * 3.14159265358979f / 180.0f;
            float cs = cosf(t), sn = sinf(t);
            float px = cx[c] + (cs >= 0 ? 1.0f : -1.0f) * radius *
                       powf(fabsf(cs), 2.0f / n);
            float py = cy[c] + (sn >= 0 ? 1.0f : -1.0f) * radius *
                       powf(fabsf(sn), 2.0f / n);

            if (np > 0 && fabsf(px - pts[np - 1].X) < 0.01f &&
                fabsf(py - pts[np - 1].Y) < 0.01f)
                continue;
            pts[np].X = px;
            pts[np].Y = py;
            np++;
        }
    }

    if (GdipCreatePath(FillModeAlternate, &path) != Ok)
        return NULL;

    GdipStartPathFigure(path);
    for (i = 1; i < np; i++)
        GdipAddPathLine(path, pts[i - 1].X, pts[i - 1].Y, pts[i].X, pts[i].Y);
    GdipClosePathFigure(path);
    return path;
}

static void fill_round_n(GpGraphics *g, float x, float y, float w, float h,
                         float radius, ARGB color, float n)
{
    GpPath *path;
    GpSolidFill *fill = NULL;

    if (w <= 0.5f || h <= 0.5f || ((color >> 24) & 0xFF) == 0)
        return;

    path = round_path_n(x, y, w, h, radius, n);
    if (!path)
        return;
    if (GdipCreateSolidFill(color, &fill) == Ok)
    {
        GdipFillPath(g, (GpBrush *)fill, path);
        GdipDeleteBrush((GpBrush *)fill);
    }
    GdipDeletePath(path);
}

static GpPath *round_path(float x, float y, float w, float h, float radius)
{
    return round_path_n(x, y, w, h, radius, 3.0f);
}

static void fill_round(GpGraphics *g, float x, float y, float w, float h,
                       float radius, ARGB color)
{
    fill_round_n(g, x, y, w, h, radius, color, 3.0f);
}

/* Width of a string as draw_text() will lay it out, for centring an icon and
 * a label as one group.  It must be measured with the *same* format the text
 * is drawn with: GDI+ pads the default format by 1/6 em on each side, and
 * measuring with the typographic format instead made the box a few pixels too
 * narrow, which clipped the last glyph. */
static float text_width(GpGraphics *g, const wchar_t *s, GpFont *font)
{
    GpRectF layout = { 0.0f, 0.0f, 4000.0f, 4000.0f };
    GpRectF bound;
    INT cps = 0, lines = 0;

    if (!g || !s || !*s || !font || !g_fmt_l)
        return 0.0f;
    if (GdipMeasureString(g, s, -1, font, &layout, g_fmt_l, &bound, &cps, &lines) != Ok)
        return 0.0f;
    return bound.Width;
}

static void draw_text(GpGraphics *g, const wchar_t *s, GpFont *font,
                      float x, float y, float w, float h, ARGB color,
                      GpStringFormat *fmt)
{
    GpSolidFill *br = NULL;
    GpRectF rect;

    if (!s || !*s || !font || ((color >> 24) & 0xFF) == 0)
        return;

    rect.X = x;
    rect.Y = y;
    rect.Width = w;
    rect.Height = h;

    if (GdipCreateSolidFill(color, &br) != Ok)
        return;
    GdipDrawString(g, s, -1, font, &rect, fmt ? fmt : g_fmt_l, (GpBrush *)br);
    GdipDeleteBrush((GpBrush *)br);
}

static void draw_switch(GpGraphics *g, float x, float y, float w, float h,
                        float t, const RgTheme *th, float alpha)
{
    /* Flat, saturated track with a white knob that nearly fills it: 2.5 px of
     * track showing on every side of a 31 px switch.  The earlier version had a
     * soft shadow under the knob and used the muted text accent, which read as
     * a pale grey-green pill instead of a switch that is clearly on. */
    float r = h * 0.5f;
    float inset = h * 0.081f;          /* 2.5 px on a 31 px track */
    float kr = r - inset;
    float kx = x + r + (w - h) * t;
    float ky = y + r;
    GpSolidFill *fill = NULL;

    {
        int on_a = (th->switch_on >> 24) & 0xFF;
        int off_a = (th->switch_off >> 24) & 0xFF;
        int a = (int)rg_lerp((float)off_a, (float)on_a, t);
        int rr = (int)rg_lerp((float)((th->switch_off >> 16) & 0xFF),
                              (float)((th->switch_on >> 16) & 0xFF), t);
        int gg = (int)rg_lerp((float)((th->switch_off >> 8) & 0xFF),
                              (float)((th->switch_on >> 8) & 0xFF), t);
        int bb = (int)rg_lerp((float)(th->switch_off & 0xFF),
                              (float)(th->switch_on & 0xFF), t);

        fill_round_n(g, x, y, w, h, r, rgba(a, rr, gg, bb), 2.0f);
    }

    if (GdipCreateSolidFill(fade(0xFFFFFFFF, alpha), &fill) == Ok)
    {
        GdipFillEllipse(g, (GpBrush *)fill, kx - kr, ky - kr, kr * 2.0f, kr * 2.0f);
        GdipDeleteBrush((GpBrush *)fill);
    }
}


static void slider_bounds(float *x0, float *x1)
{
    *x0 = (float)(MARGIN + PAD + sc(10, 7) + ICON_SZ + sc(12, 8));
    *x1 = (float)(MARGIN + PANEL_W - PAD - sc(4, 2));
}

static void slider_readout(const Row *r, wchar_t *out, size_t cap)
{
    if (r->percent)
        swprintf_s(out, cap, L"%d%%", (int)lroundf(r->v * 100.0f));
    else
        swprintf_s(out, cap, L"%d px", (int)lroundf(r->v));
}

static void draw_slider(GpGraphics *g, const Row *r, const RECT *rect,
                        const RgTheme *th, float alpha)
{
    float x0, x1, th_h, cy, t, kx, kr;
    wchar_t readout[32];

    slider_bounds(&x0, &x1);

    th_h = (float)sc(5, 3);
    cy = (float)rect->bottom - (float)sc(18, 12);
    kr = (float)sc(7, 5);
    if (g_drag_slider >= 0 && &g_rows[g_drag_slider] == r)
        kr *= 1.18f;

    t = (r->vmax > r->vmin) ? (r->v - r->vmin) / (r->vmax - r->vmin) : 0.0f;
    t = rg_clampf(t, 0.0f, 1.0f);
    kx = x0 + (x1 - x0) * t;

    fill_round_n(g, x0, cy - th_h * 0.5f, x1 - x0, th_h, th_h * 0.5f,
                 fade(th->track, alpha), 2.0f);
    if (kx - x0 > 1.0f)
        fill_round_n(g, x0, cy - th_h * 0.5f, kx - x0, th_h, th_h * 0.5f,
                     fade(r->enabled ? th->accent : th->track, alpha), 2.0f);

    {
        GpSolidFill *sh = NULL;
        if (GdipCreateSolidFill(fade(rgba(28, 0, 0, 0), alpha), &sh) == Ok)
        {
            GdipFillEllipse(g, (GpBrush *)sh, kx - kr, cy - kr + 1.5f,
                            kr * 2.0f, kr * 2.0f);
            GdipDeleteBrush((GpBrush *)sh);
        }
    }
    {
        GpSolidFill *fill = NULL;
        if (GdipCreateSolidFill(fade(0xFFFFFFFF, alpha), &fill) == Ok)
        {
            GdipFillEllipse(g, (GpBrush *)fill, kx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
            GdipDeleteBrush((GpBrush *)fill);
        }
    }
    {
        GpPen *pen = NULL;
        if (GdipCreatePen1(fade(th->accent, alpha), (REAL)scf(2.0f, 1.4f),
                           UnitPixel, &pen) == Ok)
        {
            GdipDrawEllipse(g, pen, kx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
            GdipDeletePen(pen);
        }
    }

    {
        float line_y = (float)rect->top + (float)sc(5, 3);
        float line_h = (float)sc(24, 17);
        float tx = (float)(rect->left + sc(10, 7)) + (float)ICON_SZ +
                   (float)sc(12, 8);

        icon_draw(g, r->icon, (float)(rect->left + sc(10, 7)) + ICON_SZ * 0.5f,
                  line_y + line_h * 0.5f, (float)ICON_SZ, fade(th->accent, alpha),
                  scf(1.9f, 1.3f));

        draw_text(g, r->label, g_font, tx, line_y,
                  (float)(rect->right - rect->left) - (tx - (float)rect->left),
                  line_h, fade(th->text, alpha), g_fmt_l);

        slider_readout(r, readout, RG_COUNT(readout));
        draw_text(g, readout, g_font_small, (float)rect->left, line_y,
                  (float)(rect->right - rect->left) - (float)sc(12, 8),
                  line_h, fade(th->dim, alpha), g_fmt_r);
    }
}

static void draw_segment(GpGraphics *g, const Row *r, int idx, const RECT *rect,
                         const RgTheme *th, float alpha)
{
    float line_y = (float)rect->top + (float)sc(4, 3);
    float line_h = (float)sc(22, 15);
    float tx = (float)(rect->left + sc(10, 7)) + (float)ICON_SZ + (float)sc(12, 8);
    float tr_x = (float)(rect->left + sc(10, 7));
    float tr_w = (float)(rect->right - rect->left) - (float)sc(20, 14);
    float tr_h = (float)sc(30, 21);
    float tr_y = (float)rect->bottom - (float)sc(8, 5) - tr_h;
    float seg_w = tr_w / (float)r->nseg;
    float thumb = g_seg_anim[idx].v;
    int i;

    icon_draw(g, r->icon, (float)(rect->left + sc(10, 7)) + ICON_SZ * 0.5f,
              line_y + line_h * 0.5f, (float)ICON_SZ, fade(th->dim, alpha),
              scf(1.9f, 1.3f));

    draw_text(g, r->label, g_font, tx, line_y,
              (float)(rect->right - rect->left) - (tx - (float)rect->left),
              line_h, fade(th->text, alpha), g_fmt_l);

    fill_round(g, tr_x, tr_y, tr_w, tr_h, tr_h * 0.5f,
               fade(menu_dark() ? rgba(30, 255, 255, 255) : rgba(18, 0, 0, 0), alpha));

    fill_round(g, tr_x + seg_w * thumb + scf(2.0f, 1.5f), tr_y + scf(2.0f, 1.5f),
               seg_w - scf(4.0f, 3.0f), tr_h - scf(4.0f, 3.0f),
               (tr_h - scf(4.0f, 3.0f)) * 0.5f, fade(th->accent, alpha));

    for (i = 0; i < r->nseg; i++)
    {
        float cx = tr_x + seg_w * (float)i;
        int sel = (i == r->seg_cur);
        /* Label brightens as the thumb arrives under it. */
        float k = 1.0f - rg_clampf(fabsf(thumb - (float)i), 0.0f, 1.0f);
        ARGB col = menu_dark()
                 ? rgba((int)(150 + 105 * k), 246, 247, 250)
                 : rgba((int)(130 + 125 * k), 26, 27, 31);
        if (sel && k > 0.9f)
            col = menu_dark() ? rgba(255, 20, 24, 32) : rgba(255, 255, 255, 255);
        draw_text(g, r->seg[i], g_font_small, cx, tr_y, seg_w, tr_h,
                  fade(col, alpha), g_fmt_c);
    }
}

/* Cancel / confirm button boxes, shared by the painter and the hit test. */
static void confirm_buttons(const RECT *rc, RECT *cancel, RECT *ok)
{
    float bw = (float)sc(66, 46);
    float bh = (float)sc(28, 20);
    float gap = (float)sc(8, 5);
    float by = (float)rc->bottom - (float)sc(8, 5) - bh;
    float bx1 = (float)rc->right - (float)sc(10, 7) - bw;
    float bx0 = bx1 - bw - gap;

    if (cancel)
    {
        cancel->left = (LONG)lroundf(bx0);
        cancel->top = (LONG)lroundf(by);
        cancel->right = (LONG)lroundf(bx0 + bw);
        cancel->bottom = (LONG)lroundf(by + bh);
    }
    if (ok)
    {
        ok->left = (LONG)lroundf(bx1);
        ok->top = (LONG)lroundf(by);
        ok->right = (LONG)lroundf(bx1 + bw);
        ok->bottom = (LONG)lroundf(by + bh);
    }
}

static void draw_confirm(GpGraphics *g, int idx, const RECT *rect,
                         const RgTheme *th, float alpha)
{
    float t = confirm_open();
    float pad_x = (float)sc(12, 8);
    float gy = (float)sc(3, 2);     /* air above and below the plate */
    float line_y = (float)rect->top + gy + (float)sc(7, 5);
    float line_h = (float)sc(22, 15);
    float icon_x = (float)rect->left + pad_x;
    float tx = icon_x + (float)ICON_SZ + (float)sc(12, 8);
    float text_w = (float)rect->right - (float)sc(12, 8) - tx;
    wchar_t hint[160];
    wchar_t sz[64];

    shell_format_size(g_host->size, sz, RG_COUNT(sz));
    swprintf_s(hint, RG_COUNT(hint), T(S_CONFIRM_HINT), g_host->count, sz);

    /* The prompt sits on its own raised plate: it reads as a step inside the
     * menu, not as one more row. */
    fill_round(g, (float)rect->left, (float)rect->top + gy,
               (float)(rect->right - rect->left),
               (float)(rect->bottom - rect->top) - gy * 2.0f,
               (float)ROW_R + scf(4.0f, 2.5f),
               fade(menu_dark() ? rgba(26, 255, 255, 255)
                                : rgba(22, 0, 0, 0), alpha * t));

    if (g_page == PAGE_DELETE)
    {
        /* On its own page the row above already carries the title, so the plate
         * holds one line: the icon and the text travel together as a group and
         * the group is centred in the space above the buttons - left-aligning
         * it left a hole on the right of the plate. */
        RECT ca, ok;
        float gap = (float)sc(12, 8);
        float tw = text_width(g, hint, g_font);
        float group = (float)ICON_SZ + gap + tw;
        float top = (float)rect->top + gy;
        float bottom;
        float cy;

        confirm_buttons(rect, &ca, &ok);
        bottom = (float)ca.top;
        cy = (top + bottom) * 0.5f;

        if (tw <= 0.0f)
        {
            /* Measurement failed: fall back to a plain centred line. */
            icon_draw(g, IC_TRASH, (float)rect->left + pad_x + (float)ICON_SZ * 0.5f,
                      cy, (float)ICON_SZ, fade(th->accent, alpha * t),
                      scf(1.9f, 1.3f));
            draw_text(g, hint, g_font, (float)rect->left, cy - line_h * 0.5f,
                      (float)(rect->right - rect->left), line_h,
                      fade(th->text, alpha * t), g_fmt_c);
        }
        else
        {
            float gx = (float)rect->left +
                       ((float)(rect->right - rect->left) - group) * 0.5f;
            float min_x = (float)rect->left + pad_x;

            if (gx < min_x)
                gx = min_x;

            icon_draw(g, IC_TRASH, gx + (float)ICON_SZ * 0.5f, cy, (float)ICON_SZ,
                      fade(th->accent, alpha * t), scf(1.9f, 1.3f));
            draw_text(g, hint, g_font, gx + (float)ICON_SZ + gap,
                      cy - line_h * 0.5f, tw + 2.0f, line_h,
                      fade(th->text, alpha * t), g_fmt_l);
        }
    }
    else
    {
        icon_draw(g, IC_TRASH, icon_x + (float)ICON_SZ * 0.5f,
                  line_y + line_h * 0.5f, (float)ICON_SZ,
                  fade(th->accent, alpha * t), scf(1.9f, 1.3f));

        draw_text(g, T(S_CONFIRM_TITLE), g_font, tx, line_y, text_w, line_h,
                  fade(th->text, alpha * t), g_fmt_l);
        draw_text(g, hint, g_font_small, tx, line_y + line_h,
                  text_w, (float)sc(20, 14), fade(th->dim, alpha * t), g_fmt_l);
    }

    {
        RECT ca, ok;
        float bh = (float)sc(28, 20);
        float bw = (float)sc(66, 46);

        confirm_buttons(rect, &ca, &ok);

        fill_round(g, (float)ca.left, (float)ca.top, bw, bh, bh * 0.5f,
                   fade(menu_dark() ? rgba(g_confirm_hover == 1 ? 58 : 30, 255, 255, 255)
                                    : rgba(g_confirm_hover == 1 ? 40 : 24, 0, 0, 0),
                        alpha * t));
        draw_text(g, T(S_CONFIRM_CANCEL), g_font_small, (float)ca.left,
                  (float)ca.top, bw, bh, fade(th->text, alpha * t), g_fmt_c);

        /* The destructive button is the only saturated thing in the panel, so
         * it pulls the eye without needing a second colour. */
        fill_round(g, (float)ok.left, (float)ok.top, bw, bh, bh * 0.5f,
                   fade(th->accent, alpha * t));
        draw_text(g, T(S_BTN_EMPTY), g_font_small, (float)ok.left,
                  (float)ok.top, bw, bh,
                  fade(menu_dark() ? rgba(255, 22, 30, 40) : rgba(255, 255, 255, 255),
                       alpha * t), g_fmt_c);
    }

    (void)idx;
}

static void ensure_backdrop(void)
{
    int bx = g_win_x + MARGIN;
    int by = g_win_y + MARGIN;
    int bw = PANEL_W;
    int bh = rows_height();
    int q = (int)lroundf(g_host->cfg->blur_radius);
    /* Capture taller than the panel needs: the "empty the bin?" prompt grows
     * the panel while it animates, and re-grabbing the screen every frame of
     * that animation is exactly the hitch it looks like. */
    int cap_h = bh + CONFIRM_EXTRA + 8;
    RgBitmap fresh;

    if (g_bd_valid && g_bd_x == bx && g_bd_y == by &&
        g_bd.w == bw && g_bd.h == bh && g_bd_q == q)
        return;

    if (g_live)
    {
        if (!g_bd_raw.px || g_bd_raw_x != bx || g_bd_raw_y != by ||
            g_bd_raw.w != bw || g_bd_raw.h < cap_h)
        {
            glass_release(&g_bd_raw);
            glass_release(&g_bd_blur);
            g_bd_raw_q = -1;
            if (glass_capture(bx, by, bw, cap_h, &g_bd_raw))
            {
                g_bd_raw_x = bx;
                g_bd_raw_y = by;
            }
        }

        if (g_bd_raw.px && g_bd_raw_q != q)
        {
            glass_release(&g_bd_blur);
            if (glass_blurred_copy(&g_bd_raw, (float)q, &g_bd_blur))
                g_bd_raw_q = q;
        }

        if (g_bd_blur.px)
        {
            memset(&fresh, 0, sizeof(fresh));
            if (glass_crop(&g_bd_blur, bw, bh, &fresh))
            {
                glass_release(&g_bd);
                g_bd = fresh;
                g_bd_x = bx;
                g_bd_y = by;
                g_bd_q = q;
                g_bd_valid = TRUE;
                return;
            }
        }
    }

    /* No capture available (wallpaper mode, or the desktop is not
     * capturable): blur the wallpaper image instead. */
    memset(&fresh, 0, sizeof(fresh));
    if (glass_backdrop(bx, by, bw, bh, (float)q, &fresh))
    {
        glass_release(&g_bd);
        g_bd = fresh;
        g_bd_x = bx;
        g_bd_y = by;
        g_bd_q = q;
        g_bd_valid = TRUE;
    }
}

/* Multiplies the finished surface by `k` so the whole panel can fade in. */
static void fade_surface(int w, int h, float k)
{
    BYTE *p = (BYTE *)g_surf.bits;
    int i, n = w * h;

    if (!p || k >= 0.999f)
        return;

    for (i = 0; i < n; i++)
    {
        p[0] = (BYTE)(p[0] * k);
        p[1] = (BYTE)(p[1] * k);
        p[2] = (BYTE)(p[2] * k);
        p[3] = (BYTE)(p[3] * k);
        p += 4;
    }
}


static void render_menu(void)
{
    int ph, pw, W, H;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    RgTheme th;
    float open = g_open.v;
    float page = g_page_anim.v;
    int   dx = (int)lroundf((1.0f - page) * (float)g_page_dir * (float)sc(18, 12));
    int y, i;

    if (!g_host || !g_hwnd)
        return;

    relayout();

    ph = rows_height();
    pw = PANEL_W;
    W = pw + MARGIN * 2;
    H = ph + MARGIN * 2;

    if (!render_surface_resize(&g_surf, W, H))
        return;

    ensure_backdrop();

    memset(g_surf.bits, 0, (size_t)W * H * 4);

    render_glass_panel(&g_surf, g_bd_valid ? &g_bd : NULL,
                       MARGIN, MARGIN, pw, ph, RADIUS,
                       g_host->cfg->opacity, g_host->cfg->material,
                       menu_dark());

    /* The theme row carries a sun/moon icon, and the theme can change while the
     * menu is open.  Sync it here rather than at build_rows() time, otherwise
     * the icon only catches up on the next page change - which reads as the
     * theme applying one beat late. */
    {
        int k, i;
        for (k = 0; k < g_nrows; k++)
        {
            if (g_rows[k].kind != ROW_SEGMENT)
                continue;
            for (i = 0; i < g_rows[k].nseg; i++)
            {
                int c = g_rows[k].seg_cmd[i];
                if (c == RG_CMD_THEME_LIGHT || c == RG_CMD_THEME_DARK)
                {
                    g_rows[k].icon = menu_dark() ? IC_MOON : IC_SUN;
                    break;
                }
            }
        }
    }

    render_theme(menu_dark(), &th);

    if (GdipCreateBitmapFromScan0(W, H, W * 4, PixelFormat32bppPARGB,
                                  g_surf.bits, &bmp) != Ok)
        return;
    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetCompositingMode(g, CompositingModeSourceOver);

    /* Rows are clipped to the panel so the page slide cannot spill out. */
    {
        GpPath *clip = round_path((float)MARGIN, (float)MARGIN,
                                  (float)pw, (float)ph, (float)RADIUS);
        if (clip)
        {
            GdipSetClipPath(g, clip, CombineModeReplace);
            GdipDeletePath(clip);
        }
    }

    y = MARGIN + PAD;

    for (i = 0; i < g_nrows; i++)
    {
        Row *r = &g_rows[i];
        int rh = row_height(r);
        RECT rc;
        float alpha = page;

        rc.left = MARGIN + PAD + dx;
        rc.top = y;
        rc.right = MARGIN + pw - PAD + dx;
        rc.bottom = y + rh;
        g_rowrect[i] = rc;

        if (r->kind == ROW_SEP)
        {
            GpPen *pen = NULL;
            if (GdipCreatePen1(fade(th.border, alpha), 1.0f, UnitPixel, &pen) == Ok)
            {
                float ly = (float)y + (float)rh * 0.5f;
                GdipDrawLine(g, pen, (float)(MARGIN + PAD + sc(8, 5)) + (float)dx, ly,
                             (float)(MARGIN + pw - PAD - sc(8, 5)) + (float)dx, ly);
                GdipDeletePen(pen);
            }
            y += rh;
            continue;
        }

        /* The prompt paints its own plate; a hover wash on top of it would
         * double up and make the block flicker as the pointer crosses it. */
        if (r->kind != ROW_CONFIRM)
        {
            float hv = g_hover_anim[i].v;
            if (hv > 0.004f)
                fill_round(g, (float)rc.left, (float)rc.top,
                           (float)(rc.right - rc.left), (float)rh,
                           (float)ROW_R, fade(th.hover_bg, alpha * hv));
        }

        if (r->kind == ROW_GAP)
        {
            y += rh;
            continue;
        }

        if (r->kind == ROW_SLIDER)
        {
            draw_slider(g, r, &rc, &th, alpha);
            y += rh;
            continue;
        }
        if (r->kind == ROW_SEGMENT)
        {
            draw_segment(g, r, i, &rc, &th, alpha);
            y += rh;
            continue;
        }
        if (r->kind == ROW_CONFIRM)
        {
            draw_confirm(g, i, &rc, &th, alpha);
            y += rh;
            continue;
        }

        {
            ARGB lc = r->enabled ? th.text
                                 : (menu_dark() ? rgba(110, 150, 156, 168)
                                                : rgba(110, 150, 152, 162));
            ARGB ic = r->enabled ? th.dim : rgba(80, 150, 152, 162);
            float iy = (float)rc.top + ((float)rh - (float)ICON_SZ) * 0.5f;
            float ix = (float)rc.left + (float)sc(10, 7);
            float tx = ix + (float)ICON_SZ + (float)sc(12, 8);

            if (r->kind == ROW_BACK)
            {
                ic = th.accent;
                lc = th.text;
            }
            else if (r->kind == ROW_PAGE)
            {
                ic = th.accent;
            }

            icon_draw(g, r->icon, ix + (float)ICON_SZ * 0.5f,
                      iy + (float)ICON_SZ * 0.5f, (float)ICON_SZ, fade(ic, alpha),
                      scf(1.9f, 1.3f));

            draw_text(g, r->label, g_font, tx, (float)rc.top,
                      (float)(rc.right - rc.left) - (tx - (float)rc.left),
                      (float)rh, fade(lc, alpha), g_fmt_l);

            if (r->kind == ROW_TOGGLE)
            {
                float shh = (float)sc(25, 18);
                float sw = shh * 1.68f;
                draw_switch(g, (float)rc.right - (float)sc(10, 7) - sw,
                            (float)rc.top + ((float)rh - shh) * 0.5f,
                            sw, shh, g_sw_anim[i].v, &th, alpha);
            }
            else if (r->kind == ROW_PAGE)
            {
                float cs = (float)sc(14, 10);
                float cx = (float)rc.right - (float)sc(10, 7) - cs * 0.5f;
                icon_draw(g, IC_CHEVRON_RIGHT, cx, (float)rc.top + (float)rh * 0.5f,
                          cs, fade(th.dim, alpha), scf(1.9f, 1.3f));
            }
        }

        y += rh;
    }

    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);

    fade_surface(W, H, open);

    {
        SIZE size;
        POINT src, dst;
        BLENDFUNCTION bf;
        size.cx = W;
        size.cy = H;
        src.x = 0;
        src.y = 0;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        /* Slide up while fading in: one UpdateLayeredWindow does both. */
        dst.x = g_win_x;
        dst.y = g_win_y + (int)lroundf((1.0f - open) * (float)sc(10, 6));
        UpdateLayeredWindow(g_hwnd, NULL, &dst, &size, g_surf.dc, &src, 0,
                            &bf, ULW_ALPHA);
    }
}

/* -------------------------------------------------------------- behaviour */

static int hit_row(int x, int y)
{
    int i;
    for (i = 0; i < g_nrows; i++)
    {
        if (g_rows[i].kind == ROW_SEP)
            continue;
        if (x >= g_rowrect[i].left && x <= g_rowrect[i].right &&
            y >= g_rowrect[i].top && y <= g_rowrect[i].bottom)
            return i;
    }
    return -1;
}

static void slider_from_x(int idx, int x)
{
    Row *r = &g_rows[idx];
    float x0, x1, t, v;

    if (r->kind != ROW_SLIDER)
        return;

    slider_bounds(&x0, &x1);
    t = (x1 > x0) ? ((float)x - x0) / (x1 - x0) : 0.0f;
    t = rg_clampf(t, 0.0f, 1.0f);

    v = r->vmin + (r->vmax - r->vmin) * t;
    v = r->percent ? floorf(v * 100.0f + 0.5f) / 100.0f : floorf(v + 0.5f);
    v = rg_clampf(v, r->vmin, r->vmax);

    if (fabsf(v - r->v) < 1e-4f)
        return;

    r->v = v;
    if (g_host->on_preview)
        g_host->on_preview(r->id, v);
    render_menu();
}

static void present(int x, int y);

/* Which segment of a segmented row is under the pointer. */
static int hit_segment(int idx, int x)
{
    const Row *r = &g_rows[idx];
    const RECT *rc = &g_rowrect[idx];
    float tr_x = (float)(rc->left + sc(10, 7));
    float tr_w = (float)(rc->right - rc->left) - (float)sc(20, 14);
    float seg_w = tr_w / (float)r->nseg;
    int i;

    for (i = 0; i < r->nseg; i++)
        if ((float)x >= tr_x + seg_w * (float)i &&
            (float)x < tr_x + seg_w * (float)(i + 1))
            return i;
    return -1;
}

static void start_anim_timer(void)
{
    if (g_hwnd)
        SetTimer(g_hwnd, 2, 8, NULL);
}

static void activate(int idx)
{
    Row *r;

    if (idx < 0 || idx >= g_nrows)
        return;

    r = &g_rows[idx];
    if (!r->enabled)
        return;

    switch (r->kind)
    {
    case ROW_CMD:
        if (r->id == RG_CMD_EMPTY && g_host->count > 0)
        {
            /* The row grows into the "empty the bin?" prompt in place: same
             * panel, same glass, no second window to keep in sync. */
            g_confirm_row = idx;
            g_confirm_hover = 0;
            g_hover = -1;
            build_rows();
            anim_to(&g_confirm_anim, 1.0f, 180);
            present(g_win_x, g_win_y);
            start_anim_timer();
            break;
        }
        menu_hide();
        g_host->on_command(r->id);
        break;

    case ROW_CONFIRM:
        /* Only the two buttons act; a stray click keeps the prompt open. */
        break;

    case ROW_TOGGLE:
        r->checked = !r->checked;
        anim_to(&g_sw_anim[idx], r->checked ? 1.0f : 0.0f, 170);
        g_host->on_command(r->id);
        start_anim_timer();
        break;

    case ROW_PAGE:
        g_page = r->id;
        g_page_dir = 1;
        goto repage;

    case ROW_BACK:
        g_page = 0;
        g_page_dir = -1;
    repage:
        g_confirm_row = -1;
        g_confirm_anim.v = 0.0f;
        g_hover = -1;
        build_rows();
        g_page_anim.v = 0.0f;
        anim_to(&g_page_anim, 1.0f, 190);
        present(g_win_x, g_win_y);
        start_anim_timer();
        break;

    default:
        break;
    }
}

/* Clicking a segment applies that option directly - no cycling. */
static void activate_segment(int idx, int seg)
{
    Row *r = &g_rows[idx];

    if (seg < 0 || seg >= r->nseg || seg == r->seg_cur)
        return;

    r->seg_cur = seg;
    anim_to(&g_seg_anim[idx], (float)seg, 190);
    if (r->seg_cmd[seg])
        g_host->on_command(r->seg_cmd[seg]);
    start_anim_timer();
}

static LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
    {
        int idx;

        if (g_drag_slider >= 0)
        {
            /* A 1 kHz mouse sends a move every millisecond, and each one costs a
             * full menu repaint plus a card repaint.  Drain the queue, take the
             * newest x, and paint at most ~60 times a second. */
            MSG queued;
            ULONGLONG now;
            int x = GET_X_LPARAM(lp);

            while (PeekMessageW(&queued, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE))
                x = GET_X_LPARAM(queued.lParam);

            now = GetTickCount64();
            if (now - g_slider_tick < 14)
                return 0;
            g_slider_tick = now;

            slider_from_x(g_drag_slider, x);
            return 0;
        }

        idx = hit_row(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        {
            int ch = 0;
            if (idx >= 0 && g_rows[idx].kind == ROW_CONFIRM)
            {
                RECT ca, ok;
                POINT pt;
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
                confirm_buttons(&g_rowrect[idx], &ca, &ok);
                if (PtInRect(&ok, pt)) ch = 2;
                else if (PtInRect(&ca, pt)) ch = 1;
            }
            if (ch != g_confirm_hover)
            {
                g_confirm_hover = ch;
                render_menu();
            }
        }

        if (idx != g_hover)
        {
            int i;
            g_hover = idx;
            for (i = 0; i < g_nrows; i++)
                anim_to(&g_hover_anim[i],
                        (i == idx && g_rows[i].enabled) ? 1.0f : 0.0f, 110);
            start_anim_timer();
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        int idx;

        idx = hit_row(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx >= 0 && g_rows[idx].kind == ROW_SLIDER && g_rows[idx].enabled)
        {
            g_drag_slider = idx;
            g_drag_slider_final = idx;
            g_slider_tick = 0;
            SetCapture(hwnd);
            slider_from_x(idx, GET_X_LPARAM(lp));
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int idx;

        if (g_drag_slider >= 0)
        {
            int last = g_drag_slider_final;
            g_drag_slider = -1;
            /* Apply the position under the pointer, even if the last move was
             * rate limited away, then hand the final value to the host so the
             * card repaints once. */
            slider_from_x(last, GET_X_LPARAM(lp));
            if (last >= 0 && last < g_nrows && g_host->on_set)
                g_host->on_set(g_rows[last].id, g_rows[last].v);
            render_menu();
            return 0;
        }

        idx = hit_row(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (idx >= 0 && g_rows[idx].kind == ROW_CONFIRM)
        {
            RECT ca, ok;
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            confirm_buttons(&g_rowrect[idx], &ca, &ok);

            if (PtInRect(&ok, pt))
            {
                menu_hide();
                g_host->on_command(RG_CMD_EMPTY_NOW);
            }
            else if (PtInRect(&ca, pt))
            {
                g_confirm_hover = 0;
                g_hover = -1;
                g_page = PAGE_MAIN;
                g_page_dir = -1;
                build_rows();
                g_page_anim.v = 0.0f;
                anim_to(&g_page_anim, 1.0f, 190);
                present(g_win_x, g_win_y);
                start_anim_timer();
            }
            return 0;
        }

        if (idx >= 0 && g_rows[idx].kind == ROW_SEGMENT)
            activate_segment(idx, hit_segment(idx, GET_X_LPARAM(lp)));
        else if (idx >= 0)
            activate(idx);
        else
        {
            /* A click on a separator or in the panel padding is not a dismiss:
             * the panel is a surface, only the area outside it closes it. */
            RECT pr;
            POINT pt;
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            pr.left = MARGIN;
            pr.top = MARGIN;
            pr.right = MARGIN + PANEL_W;
            pr.bottom = MARGIN + rows_height();
            if (!PtInRect(&pr, pt))
                menu_hide();
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == 1 && g_drag_slider < 0)
        {
            POINT pt;

            if (GetAsyncKeyState(VK_ESCAPE) & 0x8001)
            {
                menu_hide();
                return 0;
            }

            if (GetTickCount64() - g_shown_at > 220 &&
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000 ||
                 GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            {
                GetCursorPos(&pt);
                {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    if (!PtInRect(&wr, pt))
                        menu_hide();
                }
            }
        }
        else if (wp == 2)
        {
            BOOL busy = FALSE;
            int i;

            busy |= anim_step(&g_open);
            busy |= anim_step(&g_page_anim);
            busy |= anim_step(&g_confirm_anim);
            for (i = 0; i < g_nrows; i++)
            {
                busy |= anim_step(&g_hover_anim[i]);
                busy |= anim_step(&g_sw_anim[i]);
                busy |= anim_step(&g_seg_anim[i]);
            }

            render_menu();

            if (g_closing && !g_open.active)
            {
                g_closing = FALSE;
                g_shown = FALSE;
                KillTimer(hwnd, 2);
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }

            if (!busy)
                KillTimer(hwnd, 2);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (g_shown && g_drag_slider < 0)
            menu_hide();
        return 0;

    case WM_DESTROY:
        g_shown = FALSE;
        g_drag_slider = -1;
        g_hwnd = NULL;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ public */

void menu_init(HINSTANCE inst, MenuHost *host)
{
    WNDCLASSEXW wc;
    static const wchar_t *names[] = { L"Microsoft YaHei UI", L"Segoe UI" };
    int i;

    g_host = host;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = menu_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = MENU_CLASS;
    RegisterClassExW(&wc);

    for (i = 0; i < RG_COUNT(names) && !g_family; i++)
    {
        GpFontFamily *fam = NULL;
        if (GdipCreateFontFamilyFromName(names[i], NULL, &fam) == Ok && fam)
            g_family = fam;
    }
    if (!g_family)
        GdipGetGenericFontFamilySansSerif(&g_family);
}

static void ensure_fonts(void)
{
    if (g_font) { GdipDeleteFont(g_font); g_font = NULL; }
    if (g_font_small) { GdipDeleteFont(g_font_small); g_font_small = NULL; }
    if (g_fmt_l) { GdipDeleteStringFormat(g_fmt_l); g_fmt_l = NULL; }
    if (g_fmt_r) { GdipDeleteStringFormat(g_fmt_r); g_fmt_r = NULL; }
    if (g_fmt_c) { GdipDeleteStringFormat(g_fmt_c); g_fmt_c = NULL; }

    GdipCreateFont(g_family, (REAL)FS, FontStyleRegular, UnitPixel, &g_font);
    GdipCreateFont(g_family, (REAL)sc(14, 10), FontStyleRegular, UnitPixel,
                   &g_font_small);

    GdipCreateStringFormat(0, 0, &g_fmt_l);
    GdipSetStringFormatAlign(g_fmt_l, StringAlignmentNear);
    GdipSetStringFormatLineAlign(g_fmt_l, StringAlignmentCenter);
    GdipSetStringFormatFlags(g_fmt_l, StringFormatFlagsNoWrap |
                                       StringFormatFlagsNoClip);

    GdipCreateStringFormat(0, 0, &g_fmt_r);
    GdipSetStringFormatAlign(g_fmt_r, StringAlignmentFar);
    GdipSetStringFormatLineAlign(g_fmt_r, StringAlignmentCenter);
    GdipSetStringFormatFlags(g_fmt_r, StringFormatFlagsNoWrap |
                                       StringFormatFlagsNoClip);

    GdipCreateStringFormat(0, 0, &g_fmt_c);
    GdipSetStringFormatAlign(g_fmt_c, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(g_fmt_c, StringAlignmentCenter);
    GdipSetStringFormatFlags(g_fmt_c, StringFormatFlagsNoWrap |
                                       StringFormatFlagsNoClip);
}

static void ensure_window(void)
{
    if (g_hwnd)
        return;

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                             WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             MENU_CLASS, L"", WS_POPUP,
                             0, 0, PANEL_W, 100, NULL, NULL,
                             GetModuleHandleW(NULL), NULL);

    if (g_hwnd && wcscmp(g_host->cfg->blur_mode, L"wallpaper") != 0 &&
        glass_live_available())
        g_live = SetWindowDisplayAffinity(g_hwnd, 0x00000011) != 0;
}

/* Positions, resizes and paints the panel at (x, y), clamped to the monitor. */
static void present(int x, int y)
{
    RECT wa;
    POINT pt;
    HMONITOR mon;
    MONITORINFO mi;
    int w, h;

    relayout();
    ensure_fonts();

    w = PANEL_W + MARGIN * 2;
    h = rows_height() + MARGIN * 2;

    ensure_window();
    if (!g_hwnd)
        return;

    pt.x = x;
    pt.y = y;
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        wa = mi.rcWork;
    else
    {
        wa.left = 0;
        wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    if (x + w > wa.right)  x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;

    g_win_x = x;
    g_win_y = y;

    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    render_menu();

    g_shown = TRUE;
    if (GetCapture() != g_hwnd)
        SetCapture(g_hwnd);
    SetTimer(g_hwnd, 1, 15, NULL);
}

void menu_show(int screen_x, int screen_y)
{
    if (!g_host)
        return;

    g_page = 0;
    g_hover = -1;
    g_drag_slider = -1;
    g_confirm_row = -1;
    g_confirm_hover = 0;
    g_confirm_anim.v = 0.0f;
    g_page_anim.v = 1.0f;
    g_page_dir = 1;
    g_closing = FALSE;
    if (!g_shown)
        g_open.v = 0.0f;

    relayout();
    ensure_fonts();
    build_rows();
    glass_live_menu(TRUE);      /* the menu owns the cache while it is open */
    glass_live_invalidate();
    g_bd_valid = FALSE;

    /* The screen may have changed since the last time the panel was up, so the
     * raw capture is dropped and re-taken on the first paint. */
    glass_release(&g_bd_raw);
    glass_release(&g_bd_blur);
    g_bd_raw_q = -1;
    present(screen_x, screen_y);

    /* The panel opens right under the pointer, so the row beneath it never gets
     * a WM_MOUSEMOVE - without this it would sit there without a hover
     * highlight until the pointer twitches. */
    {
        POINT pt;
        int idx, i;

        for (i = 0; i < g_nrows; i++)
            g_hover_anim[i].v = 0.0f;

        GetCursorPos(&pt);
        ScreenToClient(g_hwnd, &pt);
        idx = hit_row(pt.x, pt.y);
        if (idx >= 0 && g_rows[idx].enabled)
        {
            g_hover = idx;
            anim_to(&g_hover_anim[idx], 1.0f, 1);
        }
    }

    g_shown_at = GetTickCount64();
    anim_to(&g_open, 1.0f, 170);
    start_anim_timer();
}

/* The card's Empty button and the menu row must lead to the same prompt, so
 * this just opens the menu with that row already unfolded. */
void menu_confirm_show(int screen_x, int screen_y)
{
    menu_show(screen_x, screen_y);
    if (!g_shown)
        return;

    /* The card's 清空 button opens the prompt directly - it should not drop the
     * user on the main page and make them find it again.  The prompt lives on
     * its own page now, so this is a page change, not a row that grows. */
    g_page = PAGE_DELETE;
    g_page_dir = 1;
    g_hover = -1;
    g_confirm_hover = 0;
    build_rows();
    g_page_anim.v = 1.0f;       /* already on the page: no flip animation */
    present(g_win_x, g_win_y);
}

/* The bin count arrives asynchronously, and the prompt quotes it.  Rebuild and
 * repaint if the panel is up so it never shows a stale figure. */
void menu_data_changed(void)
{
    if (!g_shown || g_drag_slider >= 0)
        return;
    build_rows();
    present(g_win_x, g_win_y);
}

void menu_hide(void)
{
    if (!g_hwnd || !g_shown || g_closing)
        return;

    /* Fade out first; the animation timer does the actual ShowWindow. */
    g_shown = FALSE;
    g_closing = TRUE;
    g_drag_slider = -1;
    glass_live_menu(FALSE);     /* let the card own the capture again */
    KillTimer(g_hwnd, 1);
    if (GetCapture() == g_hwnd)
        ReleaseCapture();
    anim_to(&g_open, 0.0f, 120);
    start_anim_timer();
}

BOOL menu_visible(void)
{
    return g_shown;
}

void menu_shutdown(void)
{
    g_closing = FALSE;
    if (g_hwnd)
    {
        KillTimer(g_hwnd, 1);
        KillTimer(g_hwnd, 2);
    }
    g_shown = FALSE;
    if (g_hwnd)
    {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    if (g_font) { GdipDeleteFont(g_font); g_font = NULL; }
    if (g_font_small) { GdipDeleteFont(g_font_small); g_font_small = NULL; }
    if (g_fmt_l) { GdipDeleteStringFormat(g_fmt_l); g_fmt_l = NULL; }
    if (g_fmt_r) { GdipDeleteStringFormat(g_fmt_r); g_fmt_r = NULL; }
    if (g_fmt_c) { GdipDeleteStringFormat(g_fmt_c); g_fmt_c = NULL; }
    glass_release(&g_bd);
    glass_release(&g_bd_raw);
    glass_release(&g_bd_blur);
    g_bd_raw_q = -1;
    g_bd_valid = FALSE;
    render_surface_free(&g_surf);
}
