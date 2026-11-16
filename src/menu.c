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

enum { PAGE_MAIN = 0, PAGE_APPEARANCE = 1, PAGE_DELETE = 2, PAGE_MATERIAL = 3 };

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
    int            swatches;/* segmented row drawn as gradient colour dots */
    int            nseg;
    const wchar_t *seg[6];
    int            seg_cmd[6];
    int            seg_cur;
} Row;

/* ------------------------------------------------------------- animation */

static MenuHost *g_host;

typedef struct {
    float     v, from, to;
    ULONGLONG t0;
    int       dur;
    BOOL      active;
} Anim;

static void anim_to(Anim *a, float to, int dur)
{
    float motion = g_host && g_host->cfg
                 ? rg_clampf(g_host->cfg->motion, 0.0f, 1.0f) : 1.0f;

    if (a->active && fabsf(a->to - to) < 0.001f)
        return;
    if (!a->active && fabsf(a->v - to) < 0.001f)
        return;
    if (motion <= 0.01f)
    {
        a->v = to;
        a->from = to;
        a->to = to;
        a->active = FALSE;
        return;
    }
    a->from = a->v;
    a->to = to;
    a->t0 = GetTickCount64();
    a->dur = (int)lroundf((float)dur * (0.38f + 0.90f * motion));
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

static HWND       g_hwnd;
static RgSurface  g_surf;

/* The glass body (shadow + blur + tint + rim) is identical across every frame
 * of the open/close and page animations: only the rows on top change.  Painting
 * it every frame was the whole reason opening the menu stuttered - a panel-sized
 * per-pixel pass plus GDI+ shadow and rim, 60+ times a second.  It is rendered
 * once into this cache and memcpy'd afterwards. */
static BYTE *g_panel_cache;
static int   g_panel_w, g_panel_h;

typedef struct {
    int w, h, radius, dark, style, accent;
    int op, mat, refr;
    int bd_valid, bdw, bdh, bdx, bdy, bdq;
} PanelKey;

static PanelKey g_panel_key;
static BOOL     g_panel_valid;
static GpFontFamily *g_family;
static GpFont    *g_font;
static GpFont    *g_font_small;
/* Per-font ink-centre compensation (see render_text_ink_offset): GDI+ centres
 * its line box, and MiSans' box centre sits ~0.1 em below the visual centre of
 * the glyphs, which left every label 1.5-2 px high inside its row or pill. */
static float      g_ink_font;
static float      g_ink_small;
static GpStringFormat *g_fmt_l;
static GpStringFormat *g_fmt_r;
static GpStringFormat *g_fmt_c;

static Row  g_rows[24];
static int  g_nrows;
static int  g_page;
static int  g_hover = -1;

static int  g_drag_slider = -1;
static int  g_drag_slider_final;    /* row index being dragged */
static int  g_seg_hover = -1;       /* segment under the pointer in a row */
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
/* The golden spacing scale.  U0 is the panel -> content inset; U1 is the one
 * gap used between every pair of elements; U2 / U3 are its phi recessions for
 * small optical insets. */
static int  U0, U1, U2, U3;
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
    /* The menu and the card now share ONE corner radius - no more "menu is
     * 80% of the card" mismatch.  From that radius the whole spacing scale is
     * derived, so changing the slider rescales gaps and radii together. */
    float R = g_host ? g_host->cfg->corner_radius : 32.0f;
    float U = rg_phi_unit(R);

    ROW_H         = sc(44, 30);
    RADIUS        = sc((int)lroundf(R), 15);
    /* Same golden floor as the card: a small corner radius must not collapse
     * the panel's breathing room.  12 keeps the menu readable at R = 12, and
     * phi scaling still takes over once R / phi grows past it. */
    U0            = sc(rg_phi_step(U, 0), 12);  /* R / phi       */
    U1            = sc(rg_phi_step(U, 1), 4);   /* R / phi^2     */
    U2            = sc(rg_phi_step(U, 2), 3);   /* R / phi^3     */
    U3            = sc(rg_phi_step(U, 3), 2);   /* R / phi^4     */

    PAD           = U0;             /* every side of the panel: one U0 */
    SEP_H         = U1;             /* section separators: one U1      */
    FS            = sc(16, 12);
    PANEL_W       = sc(308, 208);
    ICON_SZ       = sc(19, 14);
    SLIDER_H      = sc(60, 42);
    SEG_H         = sc(76, 54);
    CONFIRM_EXTRA = sc(62, 42);
    MARGIN        = render_shadow_pad();

    /* Concentric: content inset by U0 -> radius R - U0 = R / phi^2 = U1.
     * Clamped to half a row so a normal row never turns into a capsule.
     * Floor 10: with a small panel radius the concentric result collapses
     * towards 0 and every row highlight read as a hard rectangle; a soft
     * 10 px radius keeps the rows round at any slider position. */
    ROW_R = (int)lroundf(rg_concentric((float)RADIUS, (float)PAD));
    if (ROW_R < 10)
        ROW_R = 10;
    if (ROW_R > ROW_H / 2)
        ROW_R = ROW_H / 2;
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

static int glass_index(void)
{
    if (wcscmp(g_host->cfg->glass, L"aurora") == 0) return 1;
    if (wcscmp(g_host->cfg->glass, L"mist") == 0) return 2;
    return 0;
}

static int accent_index(void)
{
    static const wchar_t *names[6] = { L"blue", L"violet", L"mint",
                                       L"peach", L"amber", L"lime" };
    int i;
    for (i = 0; i < 6; i++)
        if (wcscmp(g_host->cfg->accent, names[i]) == 0)
            return i;
    return 0;
}

static int icon_index(void)
{
    return wcscmp(g_host->cfg->icon, L"classic") == 0 ? 1 : 0;
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

        r = add(ROW_TOGGLE, RG_CMD_CAPTURE, T(S_MENU_CAPTURE), IC_CAMERA);
        r->checked = g_host->cfg->capture_visible;

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
    else if (g_page == PAGE_APPEARANCE)
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

        r = add(ROW_SEGMENT, 0, T(S_SET_ACCENT), IC_DROPLET);
        r->swatches = 1;
        r->nseg = 6;
        r->seg[0] = T(S_OPT_BLUE);
        r->seg[1] = T(S_OPT_VIOLET);
        r->seg[2] = T(S_OPT_MINT);
        r->seg[3] = T(S_OPT_PEACH);
        r->seg[4] = T(S_OPT_AMBER);
        r->seg[5] = T(S_OPT_LIME);
        r->seg_cmd[0] = RG_CMD_ACCENT_BASE;
        r->seg_cmd[1] = RG_CMD_ACCENT_BASE + 1;
        r->seg_cmd[2] = RG_CMD_ACCENT_BASE + 2;
        r->seg_cmd[3] = RG_CMD_ACCENT_BASE + 3;
        r->seg_cmd[4] = RG_CMD_ACCENT_BASE + 4;
        r->seg_cmd[5] = RG_CMD_ACCENT_BASE + 5;
        r->seg_cur = accent_index();

        r = add(ROW_SEGMENT, 0, T(S_SET_SIZE), IC_MONITOR);
        r->nseg = 3;
        r->seg[0] = T(S_OPT_SMALL);
        r->seg[1] = T(S_OPT_NORMAL);
        r->seg[2] = T(S_OPT_LARGE);
        r->seg_cmd[0] = RG_CMD_SIZE_SMALL;
        r->seg_cmd[1] = RG_CMD_SIZE_NORMAL;
        r->seg_cmd[2] = RG_CMD_SIZE_LARGE;
        r->seg_cur = size_index();

        r = add(ROW_SLIDER, RG_SET_CORNER, T(S_SET_CORNER), IC_MAXIMIZE);
        r->vmin = 12.0f;
        r->vmax = 52.0f;
        r->v = rg_clampf(g_host->cfg->corner_radius, r->vmin, r->vmax);
        r->percent = 0;

        r = add(ROW_SEGMENT, 0, T(S_SET_ICON), IC_IMAGE);
        r->nseg = 2;
        r->seg[0] = T(S_OPT_ICON_ACCENT);
        r->seg[1] = T(S_OPT_ICON_CLASSIC);
        r->seg_cmd[0] = RG_CMD_ICON_ACCENT;
        r->seg_cmd[1] = RG_CMD_ICON_CLASSIC;
        r->seg_cur = icon_index();

        add(ROW_SEP, 0, NULL, -1);
        add(ROW_PAGE, PAGE_MATERIAL, T(S_MENU_MATERIAL), IC_LAYERS);
        add(ROW_CMD, RG_CMD_RESET, T(S_MENU_RESET), IC_ROTATE_CCW);
    }
    else
    {
        r = add(ROW_BACK, PAGE_APPEARANCE, T(S_MENU_MATERIAL), IC_CHEVRON_LEFT);
        add(ROW_SEP, 0, NULL, -1);

        r = add(ROW_SEGMENT, 0, T(S_SET_GLASS), IC_GEM);
        r->nseg = 3;
        r->seg[0] = T(S_OPT_CRYSTAL);
        r->seg[1] = T(S_OPT_AURORA);
        r->seg[2] = T(S_OPT_MIST);
        r->seg_cmd[0] = RG_CMD_GLASS_CRYSTAL;
        r->seg_cmd[1] = RG_CMD_GLASS_AURORA;
        r->seg_cmd[2] = RG_CMD_GLASS_MIST;
        r->seg_cur = glass_index();

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

        r = add(ROW_SLIDER, RG_SET_REFRACTION, T(S_SET_REFRACTION), IC_TRIANGLE);
        r->vmin = 0.0f;
        r->vmax = 1.0f;
        r->v = rg_clampf(g_host->cfg->refraction, r->vmin, r->vmax);
        r->percent = 1;

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

static GpPath *round_path_n(float x, float y, float w, float h, float radius,
                            float n);

/* A line-gradient brush whose stops match the card's accent pair.  Gamma
 * correction matters here: linear RGB interpolation through a saturated pair
 * leaves a muddy midpoint on dark glass. */
static GpLineGradient *gradient_brush(float x, float y, float w, float h,
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

static void fill_round_grad_n(GpGraphics *g, float x, float y, float w, float h,
                              float radius, ARGB from, ARGB to, float n)
{
    GpPath *path;
    GpLineGradient *brush;

    if (w <= 0.5f || h <= 0.5f)
        return;
    path = round_path_n(x, y, w, h, radius, n);
    if (!path)
        return;
    brush = gradient_brush(x, y, w, h, from, to);
    if (brush)
    {
        GdipFillPath(g, (GpBrush *)brush, path);
        GdipDeleteBrush((GpBrush *)brush);
    }
    GdipDeletePath(path);
}

static void stroke_round_grad_n(GpGraphics *g, float x, float y, float w,
                                float h, float radius, float width,
                                ARGB from, ARGB to, float n)
{
    GpPath *path;
    GpLineGradient *brush;
    GpPen *pen = NULL;

    if (w <= 0.5f || h <= 0.5f || width <= 0.0f)
        return;
    path = round_path_n(x, y, w, h, radius, n);
    if (!path)
        return;
    brush = gradient_brush(x, y, w, h, from, to);
    if (brush && GdipCreatePen2((GpBrush *)brush, width, UnitPixel, &pen) == Ok)
    {
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    if (brush)
        GdipDeleteBrush((GpBrush *)brush);
    GdipDeletePath(path);
}

static void fill_ellipse_grad(GpGraphics *g, float x, float y, float w,
                              float h, ARGB from, ARGB to)
{
    GpLineGradient *brush = gradient_brush(x, y, w, h, from, to);
    if (brush)
    {
        GdipFillEllipse(g, (GpBrush *)brush, x, y, w, h);
        GdipDeleteBrush((GpBrush *)brush);
    }
}

/* Superellipse outline, same curve as the card corners. */
/* n = 2 is a true ellipse (a capsule when radius == h/2); n = 3.5 is the
 * squircle used by panels and cards.  Small radii need more angular samples
 * than large ones to keep chord error below a pixel, so the count is adaptive
 * rather than fixed.  Switches stay n = 2 because their knob is a true circle. */
#define SQUIRCLE_N RG_SQUIRCLE_N

static GpPath *round_path_n(float x, float y, float w, float h, float radius,
                            float n)
{
    int steps = (int)ceilf(radius * 1.75f);
    static const float start[4] = { 180.0f, 270.0f, 0.0f, 90.0f };
    float cx[4], cy[4];
    GpPath *path = NULL;
    GpPointF pts[4 * 65];
    int np = 0, c, i;

    if (radius * 2.0f > w) radius = w * 0.5f;
    if (radius * 2.0f > h) radius = h * 0.5f;
    if (radius < 0.0f) radius = 0.0f;
    if (n < 2.0f) n = 2.0f;
    if (steps < 16) steps = 16;
    if (steps > 64) steps = 64;

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
    /* Line2 emits the whole dense contour in one call; the previous per-edge
     * AddPathLine loop added hundreds of path operations to every repaint. */
    GdipAddPathLine2(path, pts, np);
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
    return round_path_n(x, y, w, h, radius, SQUIRCLE_N);
}

static void fill_round(GpGraphics *g, float x, float y, float w, float h,
                       float radius, ARGB color)
{
    fill_round_n(g, x, y, w, h, radius, color, SQUIRCLE_N);
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

/* Split "<count> 项 · <size>" at the middle dot.  The confirmation used to be
 * one line measured in full and then drawn at that width, so a bin with a few
 * hundred items and a four-digit size ran past the plate and the last glyphs
 * were clipped.  Breaking at the separator gives two short, readable lines. */
static int split_hint(const wchar_t *s, wchar_t *a, size_t acap,
                      wchar_t *b, size_t bcap)
{
    const wchar_t *p;
    size_t n;

    if (!s || !a || !b || acap == 0 || bcap == 0)
        return 0;
    a[0] = 0;
    b[0] = 0;
    p = wcsstr(s, L" · ");
    if (!p)
        return 0;

    n = (size_t)(p - s);
    if (n >= acap)
        n = acap - 1;
    wmemcpy(a, s, n);
    a[n] = 0;
    wcsncpy_s(b, bcap, p + 3, _TRUNCATE);
    return (a[0] && b[0]) ? 1 : 0;
}

/* Last-resort clamp: keep a string inside maxw, ending it with an ellipsis.
 * Only reached when even the two-line layout is too wide (absurd counts). */
static void text_ellipsize(GpGraphics *g, const wchar_t *s, GpFont *font,
                           float maxw, wchar_t *out, size_t cap)
{
    wchar_t buf[192];
    size_t n, max;

    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!s)
        return;
    if (text_width(g, s, font) <= maxw)
    {
        wcsncpy_s(out, cap, s, _TRUNCATE);
        return;
    }

    max = wcslen(s);
    if (max > RG_COUNT(buf) - 2)
        max = RG_COUNT(buf) - 2;
    for (n = max; n > 0; n--)
    {
        wmemcpy(buf, s, n);
        buf[n] = 0x2026;        /* … */
        buf[n + 1] = 0;
        if (text_width(g, buf, font) <= maxw)
        {
            wcsncpy_s(out, cap, buf, _TRUNCATE);
            return;
        }
    }
    wcsncpy_s(out, cap, L"\x2026", _TRUNCATE);
}

static void draw_text(GpGraphics *g, const wchar_t *s, GpFont *font,
                      float x, float y, float w, float h, ARGB color,
                      GpStringFormat *fmt)
{
    GpSolidFill *br = NULL;
    GpRectF rect;
    GpRectF bound;
    float bh = h;

    if (!s || !*s || !font || ((color >> 24) & 0xFF) == 0)
        return;

    rect.X = x;
    rect.Y = y;
    rect.Width = w;
    rect.Height = h;

    /* GDI+ centres the whole LINE BOX, leading included; with CJK fonts the
     * glyphs then hang one or two pixels above the visual centre of their
     * row / thumb.  Measure the real text height and centre THAT, and add the
     * font's ink bias so the glyph ink - not the line box - lands on the
     * optical centre. */
    if (GdipMeasureString(g, s, -1, font, &rect, fmt ? fmt : g_fmt_l,
                          &bound, NULL, NULL) == Ok && bound.Height > 0.5f)
        bh = bound.Height;

    rect.Y = y + (h - bh) * 0.5f +
             (font == g_font ? g_ink_font
                             : (font == g_font_small ? g_ink_small : 0.0f));
    rect.Height = bh;

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
    /* The knob is inset by half a U3; its radius is the track radius minus
     * that same inset, i.e. concentric with the track's corner arc. */
    float inset = fminf(h * 0.14f, (float)U3 * 0.5f);
    float kr = r - inset;
    float kx = x + r + (w - h) * t;
    float ky = y + r;
    GpSolidFill *fill = NULL;

    {
        fill_round_n(g, x, y, w, h, r, fade(th->switch_off, alpha), 2.0f);
        if (t > 0.004f)
            fill_round_grad_n(g, x, y, w, h, r,
                              fade(th->accent, alpha * t),
                              fade(th->accent_alt, alpha * t), 2.0f);
    }

    if (GdipCreateSolidFill(fade(0xFFFFFFFF, alpha), &fill) == Ok)
    {
        GdipFillEllipse(g, (GpBrush *)fill, kx - kr, ky - kr, kr * 2.0f, kr * 2.0f);
        GdipDeleteBrush((GpBrush *)fill);
    }
}


static void slider_bounds(float *x0, float *x1)
{
    *x0 = (float)(MARGIN + PAD + U1 + ICON_SZ + U1);
    *x1 = (float)(MARGIN + PANEL_W - PAD - U3);
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
    {
        if (r->enabled)
            fill_round_grad_n(g, x0, cy - th_h * 0.5f, kx - x0, th_h,
                              th_h * 0.5f, fade(th->accent, alpha),
                              fade(th->accent_alt, alpha), 2.0f);
        else
            fill_round_n(g, x0, cy - th_h * 0.5f, kx - x0, th_h, th_h * 0.5f,
                         fade(th->track, alpha), 2.0f);
    }

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
        float d = kr * 2.0f;
        ARGB from = fade(th->accent, alpha);
        ARGB to = fade(th->accent_alt, alpha);
        GpLineGradient *brush = gradient_brush(kx - kr, cy - kr, d, d, from, to);
        GpPen *pen = NULL;
        if (brush && GdipCreatePen2((GpBrush *)brush, (REAL)scf(2.0f, 1.4f),
                                    UnitPixel, &pen) == Ok)
        {
            GdipDrawEllipse(g, pen, kx - kr, cy - kr, d, d);
            GdipDeletePen(pen);
        }
        if (brush)
            GdipDeleteBrush((GpBrush *)brush);
    }

    {
        float line_y = (float)rect->top + (float)sc(5, 3);
        float line_h = (float)sc(24, 17);
        float tx = (float)(rect->left + U1) + (float)ICON_SZ +
                   (float)U1;

        icon_draw(g, r->icon, (float)(rect->left + U1) + ICON_SZ * 0.5f,
                  line_y + line_h * 0.5f, (float)ICON_SZ, fade(th->accent, alpha),
                  scf(1.9f, 1.3f));

        draw_text(g, r->label, g_font, tx, line_y,
                  (float)(rect->right - rect->left) - (tx - (float)rect->left),
                  line_h, fade(th->text, alpha), g_fmt_l);

        slider_readout(r, readout, RG_COUNT(readout));
        draw_text(g, readout, g_font_small, (float)rect->left, line_y,
                  (float)(rect->right - rect->left) - (float)U1,
                  line_h, fade(th->dim, alpha), g_fmt_r);
    }
}

static void draw_segment(GpGraphics *g, const Row *r, int idx, const RECT *rect,
                         const RgTheme *th, float alpha)
{
    float line_y = (float)rect->top + (float)sc(6, 4);
    float line_h = (float)sc(22, 15);
    float tx = (float)(rect->left + U1) + (float)ICON_SZ + (float)U1;
    float tr_x = (float)(rect->left + U1);
    float tr_w = (float)(rect->right - rect->left) - (float)(2 * U1);
    float tr_h = (float)sc(30, 21);
    float tr_y = (float)rect->bottom - (float)U2 - tr_h;
    float seg_w = tr_w / (float)r->nseg;
    float thumb = g_seg_anim[idx].v;
    int i;

    icon_draw(g, r->icon, (float)(rect->left + U1) + ICON_SZ * 0.5f,
              line_y + line_h * 0.5f, (float)ICON_SZ, fade(th->dim, alpha),
              scf(1.9f, 1.3f));

    draw_text(g, r->label, g_font, tx, line_y,
              (float)(rect->right - rect->left) - (tx - (float)rect->left),
              line_h, fade(th->text, alpha), g_fmt_l);

    if (r->swatches)
    {
        float d = fminf((float)sc(24, 17), tr_h - scf(2.0f, 1.0f));
        float cy = tr_y + tr_h * 0.5f;

        /* The selected name is shown once.  While the pointer explores the
         * row, preview that colour's name in place; the selected ring still
         * makes the applied value unambiguous. */
        {
            int label_i = (g_seg_hover >= 0 && g_seg_hover < r->nseg)
                        ? g_seg_hover : r->seg_cur;
            draw_text(g, r->seg[label_i], g_font_small, (float)rect->left,
                      line_y, (float)(rect->right - rect->left) - (float)U1,
                      line_h, fade(g_seg_hover >= 0 ? th->text : th->dim, alpha),
                      g_fmt_r);
        }

        for (i = 0; i < r->nseg; i++)
        {
            ARGB from = 0, to = 0, txt = 0;
            float cx = tr_x + seg_w * ((float)i + 0.5f);
            int hot = (i == g_seg_hover);
            float active = 1.0f - rg_clampf(fabsf(thumb - (float)i), 0.0f, 1.0f);
            float scale = 0.80f + 0.20f * fmaxf(active, hot ? 0.56f : 0.0f);
            float sd = d * scale;
            float sx = cx - sd * 0.5f;
            float sy = cy - sd * 0.5f;

            render_accent_palette(menu_dark(), i, &from, &to, &txt);
            if (active > 0.01f)
            {
                fill_ellipse_grad(g, cx - d * 0.72f, cy - d * 0.72f,
                                  d * 1.44f, d * 1.44f,
                                  fade(from, alpha * 0.22f * active),
                                  fade(to, alpha * 0.20f * active));
                stroke_round_grad_n(g, cx - d * 0.64f, cy - d * 0.64f,
                                    d * 1.28f, d * 1.28f, d * 0.64f,
                                    scf(2.0f, 1.4f),
                                    fade(from, alpha * active),
                                    fade(to, alpha * active), 2.0f);
            }
            else if (hot)
            {
                stroke_round_grad_n(g, sx - scf(3.0f, 2.0f),
                                    sy - scf(3.0f, 2.0f),
                                    sd + scf(6.0f, 4.0f),
                                    sd + scf(6.0f, 4.0f),
                                    (sd + scf(6.0f, 4.0f)) * 0.5f,
                                    scf(1.2f, 0.9f),
                                    fade(from, alpha * 0.58f),
                                    fade(to, alpha * 0.58f), 2.0f);
            }

            fill_ellipse_grad(g, sx, sy, sd, sd,
                              fade(from, alpha), fade(to, alpha));
            fill_ellipse_grad(g, sx + sd * 0.20f, sy + sd * 0.15f,
                              sd * 0.30f, sd * 0.22f,
                              rgba((int)(86 * alpha), 255, 255, 255),
                              rgba(0, 255, 255, 255));
        }
    }
    else
    {
        /* Track = level 1 radius (concentric with the panel, which is inset
         * by exactly U0).  Thumb = inset a fixed 2 px -> radius drops by the
         * same 2 px, so the arcs stay concentric.  The old thumb was inset by
         * U3 (~6 px at R = 44), which ate a third of the track height and
         * made every picker read as a cramped strip - the exact "精凑" from
         * the 4.9.0 screenshot.  A 2 px inset gives the Apple-sized pill. */
        float tins = scf(2.0f, 1.5f);
        float trr  = fmaxf((float)ROW_R - tins,
                           (tr_h - 2.0f * tins) * 0.5f - 1.0f);

        fill_round(g, tr_x, tr_y, tr_w, tr_h, (float)ROW_R,
                   fade(menu_dark() ? rgba(38, 255, 255, 255)
                                    : rgba(26, 0, 0, 0), alpha));

        /* Selected segment = a lifted, frosted NEUTRAL material.  A fully
         * saturated accent gradient here read as a plastic blue button glued
         * onto glass.  Apple's segmented control lifts a neutral surface and
         * lets colour live in a hairline / a whisper of tint, not the fill. */
        {
            float tx = tr_x + seg_w * thumb + tins;
            float ty = tr_y + tins;
            float tw = seg_w - 2.0f * tins;
            float thh = tr_h - 2.0f * tins;
            ARGB hi, lo, edge, tint, shade;

            if (menu_dark())
            {
                hi    = rgba((int)(255 * alpha * 0.27f), 240, 244, 252);
                lo    = rgba((int)(255 * alpha * 0.18f), 210, 218, 236);
                edge  = rgba((int)(255 * alpha * 0.34f), 255, 255, 255);
                shade = rgba((int)(255 * alpha * 0.22f), 0, 0, 0);
                tint  = fade(th->accent_soft, alpha * 0.70f);
            }
            else
            {
                hi    = rgba((int)(255 * alpha * 0.96f), 255, 255, 255);
                lo    = rgba((int)(255 * alpha * 0.86f), 236, 239, 246);
                edge  = rgba((int)(255 * alpha * 0.22f), 90, 100, 120);
                shade = rgba((int)(255 * alpha * 0.13f), 0, 0, 0);
                tint  = fade(th->accent_soft, alpha * 0.45f);
            }

            /* one-pixel lift, then a whisper of the chosen accent */
            fill_round_n(g, tx, ty + scf(1.5f, 1.0f), tw, thh,
                         trr, shade, SQUIRCLE_N);
            fill_round_n(g, tx, ty, tw, thh, trr, tint, SQUIRCLE_N);
            fill_round_grad_n(g, tx, ty, tw, thh, trr, hi, lo, SQUIRCLE_N);
            stroke_round_grad_n(g, tx + 0.5f, ty + 0.5f,
                                tw - 1.0f, thh - 1.0f,
                                trr - 0.5f, scf(1.0f, 0.8f),
                                edge, edge, SQUIRCLE_N);
        }

        for (i = 0; i < r->nseg; i++)
        {
            float cx = tr_x + seg_w * (float)i;
            int sel = (i == r->seg_cur);
            /* Label brightens as the thumb arrives under it; a hovered label
             * brightens part-way so the pointer always has a visible target. */
            float k = 1.0f - rg_clampf(fabsf(thumb - (float)i), 0.0f, 1.0f);
            if (i == g_seg_hover)
                k = fmaxf(k, 0.60f);
            ARGB col = menu_dark()
                     ? rgba((int)(176 + 79 * k), 246, 247, 250)
                     : rgba((int)(158 + 97 * k), 26, 27, 31);
            if (sel && k > 0.9f)
                col = menu_dark() ? rgba(255, 250, 251, 253)
                                  : rgba(255, 26, 27, 31);
            draw_text(g, r->seg[i], g_font_small, cx, tr_y, seg_w, tr_h,
                      fade(col, alpha), g_fmt_c);
        }
    }
}

/* Cancel / confirm button boxes, shared by the painter and the hit test. */
static void confirm_buttons(const RECT *rc, RECT *cancel, RECT *ok)
{
    float bw = (float)sc(66, 46);
    float bh = (float)sc(28, 20);
    float gap = (float)U2;
    float by = (float)rc->bottom - (float)U2 - bh;
    float bx1 = (float)rc->right - (float)U1 - bw;
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
    float pad_x = (float)U1;
    float gy = (float)sc(3, 2);     /* air above and below the plate */
    float line_y = (float)rect->top + gy + (float)sc(7, 5);
    float line_h = (float)sc(22, 15);
    float icon_x = (float)rect->left + pad_x;
    float tx = icon_x + (float)ICON_SZ + (float)U1;
    float text_w = (float)rect->right - (float)U1 - tx;
    wchar_t hint[160];
    wchar_t sz[64];

    shell_format_size(g_host->size, sz, RG_COUNT(sz));
    swprintf_s(hint, RG_COUNT(hint), T(S_CONFIRM_HINT), g_host->count, sz);

    /* The prompt sits on its own raised plate: it reads as a step inside the
     * menu, not as one more row. */
    fill_round(g, (float)rect->left, (float)rect->top + gy,
               (float)(rect->right - rect->left),
               (float)(rect->bottom - rect->top) - gy * 2.0f,
               (float)ROW_R,
               fade(menu_dark() ? rgba(26, 255, 255, 255)
                                : rgba(22, 0, 0, 0), alpha * t));

    if (g_page == PAGE_DELETE)
    {
        /* On its own page the row above already carries the title, so the plate
         * holds the icon plus the count/size line, centred as one group.
         *
         * The group is only centred as a SINGLE line when it truly fits: the
         * bin can hold thousands of items and several GB, and the old code
         * measured the full string, centred it, then clamped its left edge -
         * which pushed the tail of the text past the plate and clipped it.
         * If the one-line group is wider than the plate, break at the " · "
         * separator into count / size and stack them; the icon spans both
         * lines.  Beyond even that, each line is ellipsized. */
        RECT ca, ok;
        float gap = (float)U1;
        float inner = (float)(rect->right - rect->left) - pad_x * 2.0f;
        float tw = text_width(g, hint, g_font);
        float top = (float)rect->top + gy;
        float bottom;
        float cy;
        wchar_t h1[128], h2[128], e1[128], e2[128];
        int split;

        confirm_buttons(rect, &ca, &ok);
        bottom = (float)ca.top;
        cy = (top + bottom) * 0.5f;

        split = split_hint(hint, h1, RG_COUNT(h1), h2, RG_COUNT(h2));

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
        else if (tw + (float)ICON_SZ + gap <= inner)
        {
            /* One line fits: keep the tight icon + label group. */
            float group = (float)ICON_SZ + gap + tw;
            float gx = (float)rect->left +
                       ((float)(rect->right - rect->left) - group) * 0.5f;

            icon_draw(g, IC_TRASH, gx + (float)ICON_SZ * 0.5f, cy, (float)ICON_SZ,
                      fade(th->accent, alpha * t), scf(1.9f, 1.3f));
            draw_text(g, hint, g_font, gx + (float)ICON_SZ + gap,
                      cy - line_h * 0.5f, tw + 2.0f, line_h,
                      fade(th->text, alpha * t), g_fmt_l);
        }
        else if (split)
        {
            /* Two lines: "永久删除 338 项" over "404.41 MB". */
            float lh1 = line_h;
            float lh2 = (float)sc(20, 14);
            float block = lh1 + lh2;
            float w1 = text_width(g, h1, g_font);
            float w2 = text_width(g, h2, g_font_small);
            float text_w_max = inner - (float)ICON_SZ - gap;
            float group = (float)ICON_SZ + gap + fmaxf(w1, w2);
            float gx = (float)rect->left +
                       ((float)(rect->right - rect->left) - fminf(group, inner)) * 0.5f;
            float bx = gx + (float)ICON_SZ + gap;
            float by = cy - block * 0.5f;

            if (group > inner)
            {
                text_ellipsize(g, h1, g_font, text_w_max, e1, RG_COUNT(e1));
                text_ellipsize(g, h2, g_font_small, text_w_max, e2, RG_COUNT(e2));
            }
            else
            {
                wcsncpy_s(e1, RG_COUNT(e1), h1, _TRUNCATE);
                wcsncpy_s(e2, RG_COUNT(e2), h2, _TRUNCATE);
            }

            icon_draw(g, IC_TRASH, gx + (float)ICON_SZ * 0.5f, cy, (float)ICON_SZ,
                      fade(th->accent, alpha * t), scf(1.9f, 1.3f));
            draw_text(g, e1, g_font, bx, by, text_w_max + 2.0f, lh1,
                      fade(th->text, alpha * t), g_fmt_l);
            draw_text(g, e2, g_font_small, bx, by + lh1, text_w_max + 2.0f, lh2,
                      fade(th->dim, alpha * t), g_fmt_l);
        }
        else
        {
            /* No separator to break on: ellipsize the single line. */
            float text_w_max = inner - (float)ICON_SZ - gap;
            float group;
            float gx;

            text_ellipsize(g, hint, g_font, text_w_max, e1, RG_COUNT(e1));
            group = (float)ICON_SZ + gap + text_width(g, e1, g_font);
            gx = (float)rect->left +
                 ((float)(rect->right - rect->left) - fminf(group, inner)) * 0.5f;

            icon_draw(g, IC_TRASH, gx + (float)ICON_SZ * 0.5f, cy, (float)ICON_SZ,
                      fade(th->accent, alpha * t), scf(1.9f, 1.3f));
            draw_text(g, e1, g_font, gx + (float)ICON_SZ + gap,
                      cy - line_h * 0.5f, text_w_max + 2.0f, line_h,
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

        /* Permanent deletion uses a danger colour and an explicit verb, so the
         * action cannot be mistaken for a reversible "tidy up". */
        {
            ARGB d0 = menu_dark() ? rgba(255, 255, 126, 112)
                                  : rgba(255, 240, 110, 92);
            ARGB d1 = menu_dark() ? rgba(255, 198, 50, 52)
                                  : rgba(233, 209, 39, 48);
            fill_round_grad_n(g, (float)ok.left, (float)ok.top, bw, bh,
                              bh * 0.5f, fade(d0, alpha * t),
                              fade(d1, alpha * t), SQUIRCLE_N);
        }
        draw_text(g, T(S_CONFIRM_ACTION), g_font_small, (float)ok.left,
                  (float)ok.top, bw, bh,
                  fade(rgba(255, 255, 255, 255), alpha * t), g_fmt_c);
    }

    (void)idx;
}

static void ensure_backdrop(void)
{
    int rp = render_refract_pad();
    int bx = g_win_x + MARGIN - rp;
    int by = g_win_y + MARGIN - rp;
    int bw = PANEL_W + rp * 2;
    int bh = rows_height() + rp * 2;
    /* A menu is READ, so its frosted material always diffuses the desktop a
     * little even at blur = 0.  The old punch used to hide sharp window text
     * by flattening the capture; with a clean capture we get the real desktop
     * back and must supply that diffusion honestly. */
    float br = g_host->cfg->blur_radius + 12.0f;
    int q = (int)lroundf(g_host->cfg->blur_radius * 10.0f);
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
                /* glass_capture() excludes our own layered windows from the
                 * grab, so no reconstruction / punching is needed. */
                g_bd_raw_x = bx;
                g_bd_raw_y = by;
            }
        }

        if (g_bd_raw.px && g_bd_raw_q != q)
        {
            glass_release(&g_bd_blur);
            if (glass_blur_half(&g_bd_raw, br, &g_bd_blur))
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

    {
        PanelKey k;
        BYTE *nb;
        BOOL rebuild;

        memset(&k, 0, sizeof(k));
        k.w = W;  k.h = H;
        k.radius = RADIUS;
        k.dark = menu_dark() ? 1 : 0;
        k.style = glass_index();
        k.accent = accent_index();
        k.op = (int)(g_host->cfg->opacity * 1000.0f);
        k.mat = (int)(g_host->cfg->material * 1000.0f);
        k.refr = (int)(g_host->cfg->refraction * 1000.0f);
        k.bd_valid = g_bd_valid;
        k.bdw = g_bd.w;  k.bdh = g_bd.h;
        k.bdx = g_bd_x;  k.bdy = g_bd_y;
        k.bdq = g_bd_q;

        rebuild = !g_panel_valid || g_panel_w != W || g_panel_h != H ||
                  memcmp(&k, &g_panel_key, sizeof(k)) != 0;

        if (rebuild)
        {
            nb = (BYTE *)realloc(g_panel_cache, (size_t)W * H * 4);
            if (!nb)
            {
                g_panel_valid = FALSE;
                return;
            }

            memset(g_surf.bits, 0, (size_t)W * H * 4);

            /* The card is allowed to disappear into the wallpaper; a menu is
             * read, not just glanced at.  Its backdrop is baked into the
             * surface at ~96% coverage: at 62% the sharp window behind it
             * leaked straight through and the frosted blur was invisible -
             * the exact "模糊不对" from the user's screenshot.  Coverage is
             * pinned at 100%: even a 4% leak of the sharp window behind was
             * enough to read its text on a light panel.  The opacity slider
             * still controls tint density; only the leak is closed. */
            render_glass_panel(&g_surf, g_bd_valid ? &g_bd : NULL,
                               MARGIN, MARGIN, pw, ph, RADIUS,
                               rg_clampf(0.45f + g_host->cfg->opacity * 0.55f,
                                         0.62f, 1.0f),
                               rg_clampf(g_host->cfg->material + 0.20f,
                                         0.0f, 1.0f),
                               menu_dark(), glass_index(), accent_index(),
                               g_host->cfg->refraction,
                               1.0f);

            memcpy(nb, g_surf.bits, (size_t)W * H * 4);
            g_panel_cache = nb;
            g_panel_w = W;
            g_panel_h = H;
            g_panel_key = k;
            g_panel_valid = TRUE;
        }
        else if (g_panel_cache)
        {
            memcpy(g_surf.bits, g_panel_cache, (size_t)W * H * 4);
        }
    }

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

    render_theme(menu_dark(), accent_index(), &th);

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
                GdipDrawLine(g, pen, (float)(MARGIN + PAD + U2) + (float)dx, ly,
                             (float)(MARGIN + pw - PAD - U2) + (float)dx, ly);
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
            {
                float bw = scf(2.2f, 1.5f);
                float bh = fminf((float)rh - scf(12.0f, 8.0f), scf(26.0f, 18.0f));
                fill_round(g, (float)rc.left, (float)rc.top,
                           (float)(rc.right - rc.left), (float)rh,
                           (float)ROW_R, fade(th.hover_bg, alpha * hv));
                if (r->enabled)
                    fill_round_grad_n(g, (float)rc.left + scf(1.5f, 1.0f),
                                      (float)rc.top + ((float)rh - bh) * 0.5f,
                                      bw, bh, bw * 0.5f,
                                      fade(th.accent, alpha * hv),
                                      fade(th.accent_alt, alpha * hv), 2.0f);
            }
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
            float ix = (float)rc.left + (float)U1;
            float tx = ix + (float)ICON_SZ + (float)U1;

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
                draw_switch(g, (float)rc.right - (float)U1 - sw,
                            (float)rc.top + ((float)rh - shh) * 0.5f,
                            sw, shh, g_sw_anim[i].v, &th, alpha);
            }
            else if (r->kind == ROW_PAGE)
            {
                float cs = (float)sc(14, 10);
                float cx = (float)rc.right - (float)U1 - cs * 0.5f;
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
    /* Stepless: no floor() quantisation.  The readout still rounds for
     * display, but both the value and the knob follow the pointer exactly. */
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
    float tr_x = (float)(rc->left + U1);
    float tr_w = (float)(rc->right - rc->left) - (float)(2 * U1);
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
        g_page = r->id;
        g_page_dir = -1;
    repage:
        g_confirm_row = -1;
        g_confirm_anim.v = 0.0f;
        g_hover = -1;
        g_seg_hover = -1;
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

/* Diagnostic: write the current panel surface to %TEMP%\rg_menu_frame.bgra
 * (int32 width, int32 height, then premultiplied BGRA rows).  The menu is
 * WDA_EXCLUDEFROMCAPTURE, so a screen grab cannot show it - this lets the
 * corner mask be verified directly instead. */
static void menu_dump_raw(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    int hdr[2];

    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, MAX_PATH, L"rg_menu_frame.bgra");
    f = _wfopen(path, L"wb");
    if (!f)
        return;
    hdr[0] = g_surf.w;
    hdr[1] = g_surf.h;
    fwrite(hdr, sizeof(hdr[0]), 2, f);
    if (g_surf.bits && g_surf.w > 0 && g_surf.h > 0)
        fwrite(g_surf.bits, 4, (size_t)g_surf.w * g_surf.h, f);
    fclose(f);
}

static LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP + 9:   /* RG_WM_DUMP */
        menu_dump_raw();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            int idx;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            idx = hit_row(pt.x, pt.y);
            if (idx >= 0 && g_rows[idx].kind != ROW_SEP &&
                g_rows[idx].kind != ROW_GAP)
            {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE:
    {
        int idx;
        int seg = -1;

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
        if (idx >= 0 && g_rows[idx].kind == ROW_SEGMENT)
            seg = hit_segment(idx, GET_X_LPARAM(lp));
        if (seg != g_seg_hover)
        {
            g_seg_hover = seg;
            render_menu();
        }

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
                menu_sync_capture();   /* window gone: drop the fallback rect */
                glass_live_invalidate();  /* card backdrop: fresh desktop now */
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

    g_host = host;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = menu_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = MENU_CLASS;
    RegisterClassExW(&wc);
}

static void ensure_fonts(void)
{
    if (g_font) { GdipDeleteFont(g_font); g_font = NULL; }
    if (g_font_small) { GdipDeleteFont(g_font_small); g_font_small = NULL; }
    if (g_fmt_l) { GdipDeleteStringFormat(g_fmt_l); g_fmt_l = NULL; }
    if (g_fmt_r) { GdipDeleteStringFormat(g_fmt_r); g_fmt_r = NULL; }
    if (g_fmt_c) { GdipDeleteStringFormat(g_fmt_c); g_fmt_c = NULL; }

    /* One family for the whole app: bundled MiSans when present, else the
     * platform chain (render_app_family never returns NULL - it falls through
     * to the generic sans-serif). */
    if (!g_family)
        g_family = render_app_family();
    if (!g_family)
        return;

    GdipCreateFont(g_family, (REAL)FS, FontStyleRegular, UnitPixel, &g_font);
    GdipCreateFont(g_family, (REAL)sc(14, 10), FontStyleRegular, UnitPixel,
                   &g_font_small);

    g_ink_font  = render_text_ink_offset(g_font);
    g_ink_small = render_text_ink_offset(g_font_small);

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

/* Capture-visible has to cover BOTH layered windows: hiding only the card
 * still leaves the menu invisible in screenshots.  With the exclusion off,
 * every backdrop capture contains both windows, and glass.c punches their
 * remembered rects out before blurring. */
void menu_sync_capture(void)
{
    DWORD affinity;
    RECT wr;

    if (!g_hwnd || !g_host)
        return;
    affinity = (!g_host->cfg->capture_visible && g_live)
               ? 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */
               : 0;         /* WDA_NONE - screenshots include the menu */
    SetWindowDisplayAffinity(g_hwnd, affinity);

    /* Only punch the menu out of captures when it is actually IN them.
     * With WDA on, the pixels there are real desktop and punching would
     * replace them with a mirrored patch for no reason. */
    if (affinity == 0 && IsWindowVisible(g_hwnd) &&
        GetWindowRect(g_hwnd, &wr))
        glass_set_menu_rect(wr.left, wr.top,
                            wr.right - wr.left, wr.bottom - wr.top);
    else
        glass_set_menu_rect(0, 0, 0, 0);
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

    glass_set_capture_hwnd(1, g_hwnd);
    g_live = (g_host && wcscmp(g_host->cfg->blur_mode, L"wallpaper") != 0 &&
              glass_live_available());
    menu_sync_capture();
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

    /* Publish the menu rect BEFORE the first backdrop grab of this frame,
     * otherwise the panel photographs itself. */
    menu_sync_capture();

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
    g_seg_hover = -1;
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
    free(g_panel_cache);
    g_panel_cache = NULL;
    g_panel_valid = FALSE;
}
