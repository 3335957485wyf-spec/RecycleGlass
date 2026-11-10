/*
 * RecycleGlass-C — glass backdrop engine.
 *
 * The card needs one thing per frame: a blurred rectangle of whatever is
 * behind it.  Doing that literally - grab the screen, blur the rectangle - is
 * what made dragging stutter: a screen grab is frame-locked by the compositor
 * (~6 ms) and a full-resolution blur of the card costs another 4 ms, both of
 * them *per position*, i.e. every single frame of a drag.
 *
 * The unit of work here is therefore not "the rectangle", it is a **plane**:
 * a half-resolution image of a screen area that is slightly larger than the
 * card.  It is captured once, blurred once, and every frame is then a
 * bilinear crop out of it - about 0.2 ms, no matter how fast the card moves.
 * A drag only rebuilds the plane when the card walks out of the margin.
 *
 * Half resolution is not a shortcut: a blurred image has no detail above the
 * blur radius to begin with, so sampling it at 1/2 and interpolating back up
 * is visually identical while being 4x cheaper to capture and to blur.
 * Blurring the whole plane and cropping afterwards also removes the edge
 * bleeding the old crop-then-blur order produced along the card's border.
 */
#include "glass.h"

/* ------------------------------------------------------------ registry I/O */

static BOOL reg_read_str(HKEY root, const wchar_t *sub, const wchar_t *name,
                         wchar_t *out, DWORD cap)
{
    HKEY key;
    DWORD type = 0, size = cap * sizeof(wchar_t);
    LONG rc;

    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;

    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return FALSE;
    out[cap - 1] = 0;
    return TRUE;
}

/* Wallpaper settings live in the registry as REG_SZ on most systems but as
 * REG_DWORD on some; reading a REG_SZ "0" through a DWORD buffer yields 0x30
 * and silently flips the tile flag on, so the type has to be honoured. */
static int reg_read_int(const wchar_t *sub, const wchar_t *name, int dflt)
{
    HKEY key;
    DWORD type = 0, size = 0;
    BYTE buf[64];

    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return dflt;

    size = sizeof(buf);
    if (RegQueryValueExW(key, name, NULL, &type, buf, &size) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return dflt;
    }
    RegCloseKey(key);

    if (type == REG_DWORD)
        return (int)(*(DWORD *)buf);

    if (type == REG_SZ || type == REG_EXPAND_SZ)
    {
        ((wchar_t *)buf)[(sizeof(buf) / sizeof(wchar_t)) - 1] = 0;
        return _wtoi((const wchar_t *)buf);
    }

    return dflt;
}

/* --------------------------------------------------------------- wallpaper */

static GpImage *g_wall = NULL;
static wchar_t  g_wall_path[MAX_PATH];
static FILETIME g_wall_mtime;
static BOOL     g_wall_tried = FALSE;

static BOOL file_mtime(const wchar_t *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return FALSE;
    *ft = fad.ftLastWriteTime;
    return TRUE;
}

/* Set when the wallpaper file is reloaded, so cached planes are thrown away
 * instead of showing the previous image forever. */
static BOOL g_wall_dirty;
static int  g_wall_style = -1;

static void load_wallpaper(void)
{
    wchar_t path[MAX_PATH];
    FILETIME ft;

    if (!g_wall_tried)
    {
        g_wall_tried = TRUE;
        path[0] = 0;

        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) &&
            path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(g_wall_path, MAX_PATH, path);
        }
        else if (reg_read_str(HKEY_CURRENT_USER, L"Control Panel\\Desktop",
                              L"WallPaper", path, MAX_PATH) &&
                 GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(g_wall_path, MAX_PATH, path);
        }
        else
        {
            g_wall_path[0] = 0;
            return;
        }
    }

    if (g_wall_path[0] == 0)
        return;

    if (g_wall)
    {
        if (file_mtime(g_wall_path, &ft) &&
            ft.dwLowDateTime == g_wall_mtime.dwLowDateTime &&
            ft.dwHighDateTime == g_wall_mtime.dwHighDateTime)
            return; /* still fresh */
        GdipDisposeImage(g_wall);
        g_wall = NULL;
    }

    if (GdipLoadImageFromFile(g_wall_path, &g_wall) != Ok)
        g_wall = NULL;

    g_wall_dirty = TRUE;

    if (g_wall && !file_mtime(g_wall_path, &g_wall_mtime))
        memset(&g_wall_mtime, 0, sizeof(g_wall_mtime));
}

static void desktop_color(BYTE *r, BYTE *g, BYTE *b)
{
    wchar_t buf[64] = L"0 0 0";
    int rr = 0, gg = 0, bb = 0;

    reg_read_str(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background",
                 buf, RG_COUNT(buf));
    if (swscanf_s(buf, L"%d %d %d", &rr, &gg, &bb) == 3)
    {
        *r = (BYTE)rg_clampi(rr, 0, 255);
        *g = (BYTE)rg_clampi(gg, 0, 255);
        *b = (BYTE)rg_clampi(bb, 0, 255);
    }
    else
    {
        *r = *g = *b = 0;
    }
}

/* ------------------------------------------------------------- placement */

typedef struct { int x, y, w, h; } RectI;

static RectI monitor_rect_at(int x, int y)
{
    RectI r;
    POINT pt;
    HMONITOR mon;
    MONITORINFO mi;

    pt.x = x;
    pt.y = y;
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);

    if (mon && GetMonitorInfoW(mon, &mi))
    {
        r.x = mi.rcMonitor.left;
        r.y = mi.rcMonitor.top;
        r.w = mi.rcMonitor.right - mi.rcMonitor.left;
        r.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    else
    {
        r.x = 0;
        r.y = 0;
        r.w = GetSystemMetrics(SM_CXSCREEN);
        r.h = GetSystemMetrics(SM_CYSCREEN);
    }
    return r;
}

static RectI virtual_rect(void)
{
    RectI r;
    r.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (r.w <= 0) r.w = GetSystemMetrics(SM_CXSCREEN);
    if (r.h <= 0) r.h = GetSystemMetrics(SM_CYSCREEN);
    return r;
}

/* ------------------------------------------------------------------ blur
 *
 * Three box passes approximate a Gaussian closely enough that the difference
 * is invisible under a tint, and each pass is a prefix-sum subtraction - O(1)
 * per pixel regardless of radius, which is what makes a 48 px blur of a
 * 1280x800 plane cheap.  The old recursive IIR filter was exact but ran four
 * float passes over an RGB temp buffer: 4 ms on the card alone, per frame.
 */
static int   *g_prefix;
static int    g_prefix_cap;
static BYTE  *g_blur_tmp;
static size_t g_blur_tmp_cap;

static BOOL blur_scratch(int w, int h)
{
    size_t need = (size_t)w * h * 4;
    int span = (w > h ? w : h) + 1;

    if (span * 3 > g_prefix_cap)
    {
        int *nb = (int *)realloc(g_prefix, (size_t)span * 3 * sizeof(int));
        if (!nb)
            return FALSE;
        g_prefix = nb;
        g_prefix_cap = span * 3;
    }
    if (need > g_blur_tmp_cap)
    {
        BYTE *nb = (BYTE *)realloc(g_blur_tmp, need);
        if (!nb)
            return FALSE;
        g_blur_tmp = nb;
        g_blur_tmp_cap = need;
    }
    return TRUE;
}

/* Horizontal box pass: prefix sums per row, then one subtraction per pixel. */
static void box_h(const BYTE *src, BYTE *dst, int w, int h, int r)
{
    int *p0 = g_prefix, *p1 = g_prefix + w + 1, *p2 = g_prefix + (w + 1) * 2;
    int y, x;

    for (y = 0; y < h; y++)
    {
        const BYTE *s = src + (size_t)y * w * 4;
        BYTE *d = dst + (size_t)y * w * 4;

        p0[0] = p1[0] = p2[0] = 0;
        for (x = 0; x < w; x++)
        {
            p0[x + 1] = p0[x] + s[x * 4 + 0];
            p1[x + 1] = p1[x] + s[x * 4 + 1];
            p2[x + 1] = p2[x] + s[x * 4 + 2];
        }

        for (x = 0; x < w; x++)
        {
            int a = x - r, b = x + r;
            int n;
            if (a < 0) a = 0;
            if (b > w - 1) b = w - 1;
            n = b - a + 1;
            d[x * 4 + 0] = (BYTE)((p0[b + 1] - p0[a]) / n);
            d[x * 4 + 1] = (BYTE)((p1[b + 1] - p1[a]) / n);
            d[x * 4 + 2] = (BYTE)((p2[b + 1] - p2[a]) / n);
            d[x * 4 + 3] = 255;
        }
    }
}

/* Vertical box pass: the same trick down each column. */
static void box_v(const BYTE *src, BYTE *dst, int w, int h, int r)
{
    int x, y, k;
    int *const pre = g_prefix;

    for (x = 0; x < w; x++)
    {
        for (k = 0; k < 3; k++)
        {
            pre[0] = 0;
            for (y = 0; y < h; y++)
                pre[y + 1] = pre[y] + src[((size_t)y * w + x) * 4 + k];

            for (y = 0; y < h; y++)
            {
                int a = y - r, b = y + r;
                if (a < 0) a = 0;
                if (b > h - 1) b = h - 1;
                dst[((size_t)y * w + x) * 4 + k] =
                    (BYTE)((pre[b + 1] - pre[a]) / (b - a + 1));
            }
        }
        for (y = 0; y < h; y++)
            dst[((size_t)y * w + x) * 4 + 3] = 255;
    }
}

/* Blurs `px` in place.  `sigma` is in pixels; the box radius is derived from
 * the variance of three stacked boxes, so the result matches a Gaussian of
 * the same sigma to within a percent. */
static void blur_bgra(BYTE *px, int w, int h, float sigma)
{
    int r, pass;
    BYTE *tmp;
    int lim;

    if (w < 1 || h < 1 || sigma < 0.6f)
        return;

    r = (int)lroundf((sqrtf(4.0f * sigma * sigma + 1.0f) - 1.0f) * 0.5f);
    if (r < 1)
        r = 1;
    lim = (w < h ? w : h) - 1;
    if (r > lim)
        r = lim;

    if (!blur_scratch(w, h))
        return;
    tmp = g_blur_tmp;

    for (pass = 0; pass < 3; pass++)
    {
        box_h(px, tmp, w, h, r);
        box_v(tmp, px, w, h, r);
    }
}

/* ---------------------------------------------------------------- planes
 *
 * A plane is a half-resolution BGRA image covering a screen rectangle, plus
 * the radius it was blurred with.  `raw` is kept so a radius change is one
 * blur, not another screen grab.
 */
#define PLANE_SCALE   2
#define PLANE_MARGIN  80      /* device px of slack around the card */
#define LIVE_IDLE_MS  120     /* refresh a still card this often */
#define LIVE_MIN_MS   60

typedef struct {
    BYTE *raw;      /* half-res, unblurred */
    BYTE *px;       /* half-res, blurred */
    int   w, h;     /* plane size, in plane pixels */
    int   x, y;     /* screen origin */
    float radius;
    BOOL  valid;
} Plane;

static Plane g_wpp;     /* wallpaper, monitor sized */
static RectI g_wpp_rect;
static Plane g_ltp;     /* live capture tile, card + margin */
static RectI g_lrect;   /* screen area the live tile covers */
static float g_blur_scale = 1.0f;

static void plane_free(Plane *p)
{
    free(p->raw);
    free(p->px);
    memset(p, 0, sizeof(*p));
}

static BOOL plane_alloc(Plane *p, int w, int h)
{
    size_t bytes = (size_t)w * h * 4;
    BYTE *raw = (BYTE *)malloc(bytes);
    BYTE *blur = (BYTE *)malloc(bytes);

    plane_free(p);
    if (!raw || !blur)
    {
        free(raw);
        free(blur);
        return FALSE;
    }
    p->raw = raw;
    p->px = blur;
    p->w = w;
    p->h = h;
    return TRUE;
}

/* Bilinear crop of a plane into a full-resolution BGRA bitmap.  Screen
 * coordinates are clamped, so the card can hang off the plane's edge for a
 * frame without reading out of bounds. */
static BOOL plane_crop(const Plane *p, int x, int y, int w, int h, RgBitmap *out)
{
    BYTE *dst;
    int px, py;

    if (!p->valid || !p->px || w <= 0 || h <= 0 || !out)
        return FALSE;

    dst = (BYTE *)malloc((size_t)w * h * 4);
    if (!dst)
        return FALSE;

    for (py = 0; py < h; py++)
    {
        long sy = (long)(y + py - p->y) * 65536L / PLANE_SCALE;
        int iy = (int)(sy >> 16);
        int fy = (int)((sy >> 8) & 0xFF);
        int iy1;
        BYTE *drow = dst + (size_t)py * w * 4;

        if (iy < 0) { iy = 0; fy = 0; }
        if (iy > p->h - 1) { iy = p->h - 1; fy = 0; }
        iy1 = iy + 1 < p->h ? iy + 1 : iy;

        for (px = 0; px < w; px++)
        {
            long sx = (long)(x + px - p->x) * 65536L / PLANE_SCALE;
            int ix = (int)(sx >> 16);
            int fx = (int)((sx >> 8) & 0xFF);
            int ix1, k;
            const BYTE *a, *b, *c, *d;
            BYTE *o = drow + (size_t)px * 4;

            if (ix < 0) { ix = 0; fx = 0; }
            if (ix > p->w - 1) { ix = p->w - 1; fx = 0; }
            ix1 = ix + 1 < p->w ? ix + 1 : ix;

            a = p->px + ((size_t)iy * p->w + ix) * 4;
            b = p->px + ((size_t)iy * p->w + ix1) * 4;
            c = p->px + ((size_t)iy1 * p->w + ix) * 4;
            d = p->px + ((size_t)iy1 * p->w + ix1) * 4;

            for (k = 0; k < 3; k++)
            {
                int top = a[k] * (256 - fx) + b[k] * fx;
                int bot = c[k] * (256 - fx) + d[k] * fx;
                o[k] = (BYTE)((top * (256 - fy) + bot * fy) >> 16);
            }
            o[3] = 255;
        }
    }

    out->w = w;
    out->h = h;
    out->px = dst;
    return TRUE;
}

/* --------------------------------------------------- wallpaper plane build */

/* Rasterises the wallpaper into the wallpaper plane (half resolution) using
 * the same style rules the desktop does: fill, fit, centre, stretch, tile. */
static BOOL wp_plane_build(RectI rect, int style)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    BitmapData bd;
    UINT iw = 0, ih = 0;
    int pw = rect.w / PLANE_SCALE, ph = rect.h / PLANE_SCALE;

    if (!g_wall || pw <= 0 || ph <= 0)
        return FALSE;

    if (!plane_alloc(&g_wpp, pw, ph))
        return FALSE;

    if (GdipCreateBitmapFromScan0(pw, ph, pw * 4, PixelFormat32bppPARGB, NULL, &bmp) != Ok)
    {
        plane_free(&g_wpp);
        return FALSE;
    }
    if (GdipGetImageGraphicsContext(bmp, &g) != Ok)
    {
        GdipDisposeImage(bmp);
        plane_free(&g_wpp);
        return FALSE;
    }

    GdipSetInterpolationMode(g, InterpolationModeHighQualityBilinear);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHighQuality);
    GdipSetCompositingMode(g, CompositingModeSourceCopy);
    GdipGraphicsClear(g, 0xFF000000);

    GdipGetImageWidth(g_wall, &iw);
    GdipGetImageHeight(g_wall, &ih);

    if (iw > 0 && ih > 0)
    {
        int tile = reg_read_int(L"Control Panel\\Desktop", L"TileWallpaper", 0);
        int tw = (int)(iw / PLANE_SCALE), th = (int)(ih / PLANE_SCALE);

        if (tile && style != 22 && tw > 0 && th > 0)
        {
            int tx, ty;
            for (ty = 0; ty < ph; ty += th)
                for (tx = 0; tx < pw; tx += tw)
                    GdipDrawImageRectI(g, g_wall, tx, ty, tw, th);
        }
        else
        {
            float sx = (float)pw / (float)iw;
            float sy = (float)ph / (float)ih;
            float dw, dh, scale;

            switch (style)
            {
            case 2: /* stretch */
                dw = (float)pw;
                dh = (float)ph;
                break;
            case 6: /* fit */
                scale = (sx < sy) ? sx : sy;
                dw = (float)iw * scale;
                dh = (float)ih * scale;
                break;
            case 0: /* centre at 100% */
                dw = (float)iw / PLANE_SCALE;
                dh = (float)ih / PLANE_SCALE;
                break;
            case 10:
            case 22:
            default: /* fill / span */
                scale = (sx > sy) ? sx : sy;
                dw = (float)iw * scale;
                dh = (float)ih * scale;
                break;
            }

            GdipDrawImageRectI(g, g_wall,
                               (int)lroundf((pw - dw) * 0.5f),
                               (int)lroundf((ph - dh) * 0.5f),
                               (int)lroundf(dw), (int)lroundf(dh));
        }
    }

    GdipDeleteGraphics(g);

    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(bmp, NULL, ImageLockModeRead,
                           PixelFormat32bppPARGB, &bd) != Ok)
    {
        GdipDisposeImage(bmp);
        plane_free(&g_wpp);
        return FALSE;
    }
    {
        int row;
        for (row = 0; row < ph; row++)
            memcpy(g_wpp.raw + (size_t)row * pw * 4,
                   (BYTE *)bd.Scan0 + (size_t)row * bd.Stride,
                   (size_t)pw * 4);
    }
    GdipBitmapUnlockBits(bmp, &bd);
    GdipDisposeImage(bmp);

    g_wpp.x = rect.x;
    g_wpp.y = rect.y;
    g_wpp_rect = rect;
    g_wpp.valid = TRUE;
    return TRUE;
}

/* The wallpaper only changes when the user changes it, so probing the file
 * and the registry on every frame is pure waste - and this runs on every
 * frame of a drag.  Two seconds is short enough to notice a change and long
 * enough that a drag never pays for it. */
#define WP_RECHECK_MS 2000

static BOOL wp_plane_ready(float radius, int x, int y)
{
    static ULONGLONG checked_at;
    ULONGLONG now = GetTickCount64();
    RectI rect;
    BOOL recheck = !g_wpp.valid || now - checked_at > WP_RECHECK_MS;

    if (recheck)
    {
        checked_at = now;
        load_wallpaper();
        if (!g_wall)
            return FALSE;
        g_wall_style = reg_read_int(L"Control Panel\\Desktop", L"WallpaperStyle", 10);
    }
    if (!g_wall)
        return FALSE;

    /* The plane is built per monitor, so moving the card to a second display
     * rebuilds it instead of sampling the wrong wallpaper. */
    rect = (g_wall_style == 22) ? virtual_rect() : monitor_rect_at(x, y);

    if (!g_wpp.valid || g_wall_dirty ||
        g_wpp_rect.w != rect.w || g_wpp_rect.h != rect.h ||
        g_wpp_rect.x != rect.x || g_wpp_rect.y != rect.y)
    {
        if (!wp_plane_build(rect, g_wall_style))
            return FALSE;
        g_wall_dirty = FALSE;
        g_wpp.radius = -1.0f;
    }

    if (g_wpp.radius != radius && g_wpp.raw)
    {
        memcpy(g_wpp.px, g_wpp.raw, (size_t)g_wpp.w * g_wpp.h * 4);
        blur_bgra(g_wpp.px, g_wpp.w, g_wpp.h, radius / PLANE_SCALE);
        g_wpp.radius = radius;
    }
    return TRUE;
}

BOOL glass_backdrop(int x, int y, int w, int h, float blur_radius, RgBitmap *out)
{
    float r = blur_radius * g_blur_scale;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;

    if (!wp_plane_ready(r, x + w / 2, y + h / 2))
    {
        BYTE cr, cg, cb;
        int i, n = w * h;
        BYTE *px = (BYTE *)malloc((size_t)n * 4);

        if (!px)
            return FALSE;
        desktop_color(&cr, &cg, &cb);
        for (i = 0; i < n; i++)
        {
            px[i * 4 + 0] = cb;
            px[i * 4 + 1] = cg;
            px[i * 4 + 2] = cr;
            px[i * 4 + 3] = 255;
        }
        out->w = w;
        out->h = h;
        out->px = px;
        return TRUE;
    }

    return plane_crop(&g_wpp, x, y, w, h, out);
}

/* ------------------------------------------------------------ live capture */

static CRITICAL_SECTION g_live_lock;
static volatile LONG    g_gen;
static volatile LONG    g_quit;
static volatile LONG    g_menu_open;
static volatile LONG    g_target_set;
static volatile LONG    g_force;
static volatile LONG    g_hold;      /* a drag owns the frame pacing */
static HANDLE           g_wake;
static HANDLE           g_thread;
static HWND             g_notify;
static int              g_tx, g_ty, g_tw, g_th;
static float            g_tr;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK live_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_live_lock);
    return TRUE;
}

static void live_init(void)
{
    InitOnceExecuteOnce(&g_once, live_once, NULL, NULL);
}

/* Grabs a screen rectangle at full resolution. */
static BOOL capture_rect(int x, int y, int w, int h, BYTE **out_px)
{
    BITMAPINFO bi;
    HDC screen, mem;
    HBITMAP dib;
    HGDIOBJ old;
    void *bits = NULL;
    BOOL ok = FALSE;

    if (w <= 0 || h <= 0 || !out_px)
        return FALSE;

    screen = GetDC(NULL);
    if (!screen)
        return FALSE;
    mem = CreateCompatibleDC(screen);
    if (!mem)
    {
        ReleaseDC(NULL, screen);
        return FALSE;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      /* top-down, so rows are in screen order */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib && bits)
    {
        old = SelectObject(mem, dib);
        if (BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT))
        {
            size_t bytes = (size_t)w * h * 4;
            BYTE *px = (BYTE *)malloc(bytes);
            if (px)
            {
                memcpy(px, bits, bytes);
                *out_px = px;
                ok = TRUE;
            }
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }

    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

/* 2x2 box downsample: the only resampling the capture path does, and it is
 * exactly the low-pass a blurred image wants. */
static void downsample2(const BYTE *src, int sw, int sh, BYTE *dst, int dw, int dh)
{
    int x, y;

    for (y = 0; y < dh; y++)
    {
        int sy = y * 2;
        const BYTE *r0 = src + (size_t)sy * sw * 4;
        const BYTE *r1 = src + (size_t)(sy + 1 < sh ? sy + 1 : sy) * sw * 4;
        BYTE *d = dst + (size_t)y * dw * 4;

        for (x = 0; x < dw; x++)
        {
            int sx = x * 2;
            int sx1 = sx + 1 < sw ? sx + 1 : sx;
            int k;
            for (k = 0; k < 3; k++)
                d[x * 4 + k] = (BYTE)((r0[sx * 4 + k] + r0[sx1 * 4 + k] +
                                       r1[sx * 4 + k] + r1[sx1 * 4 + k] + 2) >> 2);
            d[x * 4 + 3] = 255;
        }
    }
}

/* Hands the freshly built tile to the renderer and takes the old buffers
 * back, so the caller can free them outside the lock. */
static void publish_plane(Plane *p)
{
    live_init();
    EnterCriticalSection(&g_live_lock);
    {
        Plane old = g_ltp;
        g_ltp = *p;
        *p = old;
        g_lrect.x = g_ltp.x;
        g_lrect.y = g_ltp.y;
        g_lrect.w = g_ltp.w * PLANE_SCALE;
        g_lrect.h = g_ltp.h * PLANE_SCALE;
    }
    InterlockedIncrement(&g_gen);
    LeaveCriticalSection(&g_live_lock);

    /* While the card is being dragged the UI thread is already painting one
     * frame per display refresh and reads the newest tile on its own; asking
     * it to repaint again here would double the frames and make the pacing
     * uneven. */
    if (g_notify && !InterlockedCompareExchange(&g_hold, 0, 0))
        PostMessageW(g_notify, RG_WM_REFRESH, 0, 0);
}

/* TRUE when the last tile_build() found the screen unchanged. */
static volatile LONG g_tile_same;

static BOOL tile_covers(int x, int y, int w, int h)
{
    BOOL ok;

    EnterCriticalSection(&g_live_lock);
    ok = g_ltp.valid &&
         x >= g_lrect.x && y >= g_lrect.y &&
         x + w <= g_lrect.x + g_lrect.w &&
         y + h <= g_lrect.y + g_lrect.h;
    LeaveCriticalSection(&g_live_lock);
    return ok;
}

/* Builds the live tile around (x,y,w,h) with margin, then blurs it. */
static BOOL tile_build(int x, int y, int w, int h, float radius)
{
    RectI mon = monitor_rect_at(x + w / 2, y + h / 2);
    int m = PLANE_MARGIN;
    int cx = x - m, cy = y - m, cw = w + m * 2, ch = h + m * 2;
    BYTE *full = NULL;
    Plane p;

    if (cx < mon.x) { cw -= mon.x - cx; cx = mon.x; }
    if (cy < mon.y) { ch -= mon.y - cy; cy = mon.y; }
    if (cx + cw > mon.x + mon.w) cw = mon.x + mon.w - cx;
    if (cy + ch > mon.y + mon.h) ch = mon.y + mon.h - cy;
    if (cw <= 4 || ch <= 4)
        return FALSE;

    if (!capture_rect(cx, cy, cw, ch, &full))
        return FALSE;

    memset(&p, 0, sizeof(p));
    if (!plane_alloc(&p, cw / PLANE_SCALE, ch / PLANE_SCALE))
    {
        free(full);
        return FALSE;
    }

    downsample2(full, cw, ch, p.raw, p.w, p.h);
    free(full);

    /* Nothing moved behind the card: keep the tile that is already published
     * instead of pushing an identical frame through the compositor.  This is
     * what keeps an idle desktop widget at ~1% of a core. */
    {
        BOOL same = FALSE;
        EnterCriticalSection(&g_live_lock);
        same = g_ltp.valid && g_ltp.raw && g_ltp.w == p.w && g_ltp.h == p.h &&
               g_ltp.x == cx && g_ltp.y == cy &&
               fabsf(g_ltp.radius - radius) < 0.6f &&
               memcmp(g_ltp.raw, p.raw, (size_t)p.w * p.h * 4) == 0;
        LeaveCriticalSection(&g_live_lock);
        if (same)
        {
            plane_free(&p);
            InterlockedExchange(&g_tile_same, 1);
            return TRUE;
        }
    }

    memcpy(p.px, p.raw, (size_t)p.w * p.h * 4);
    blur_bgra(p.px, p.w, p.h, radius / PLANE_SCALE);

    p.x = cx;
    p.y = cy;
    p.radius = radius;
    p.valid = TRUE;

    publish_plane(&p);
    plane_free(&p);       /* the buffers the swap handed back */
    InterlockedExchange(&g_tile_same, 0);
    return TRUE;
}

static DWORD WINAPI live_worker(void *arg)
{
    ULONGLONG last = 0;
    DWORD wait_ms = LIVE_IDLE_MS;
    (void)arg;

    for (;;)
    {
        int tx, ty, tw, th;
        float tr, cur_radius = 0.0f;
        BOOL forced, stale, have;
        ULONGLONG now;

        WaitForSingleObject(g_wake, wait_ms);

        if (InterlockedCompareExchange(&g_quit, 0, 0))
            break;
        if (InterlockedCompareExchange(&g_menu_open, 0, 0))
            continue;
        if (!InterlockedCompareExchange(&g_target_set, 0, 0))
            continue;

        EnterCriticalSection(&g_live_lock);
        tx = g_tx; ty = g_ty; tw = g_tw; th = g_th; tr = g_tr;
        have = g_ltp.valid;
        cur_radius = g_ltp.radius;
        LeaveCriticalSection(&g_live_lock);

        if (tw <= 0 || th <= 0)
            continue;

        now = GetTickCount64();
        forced = InterlockedExchange(&g_force, 0) != 0;

        if (!forced && last && now - last < LIVE_MIN_MS)
            continue;

        /* Rebuild when the tile cannot serve the card, when the radius
         * changed, when the caller forced it, or - while the card is still -
         * on the poll interval, so a video behind it still shows through.
         * After a few identical captures the interval doubles, up to 400 ms:
         * a static desktop should not cost more than a percent of a core. */
        stale = !have || !tile_covers(tx, ty, tw, th) ||
                fabsf(cur_radius - tr) > 0.01f ||
                now - last >= wait_ms;

        if (!forced && !stale)
            continue;

        if (tile_build(tx, ty, tw, th, tr))
        {
            last = now;
            if (InterlockedCompareExchange(&g_tile_same, 0, 0))
            {
                if (wait_ms < 400)
                    wait_ms *= 2;
            }
            else
            {
                wait_ms = LIVE_IDLE_MS;
            }
        }
    }

    return 0;
}

void glass_live_set_target(HWND hwnd)
{
    live_init();
    g_notify = hwnd;

    if (!g_wake)
        g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_thread && g_wake)
        g_thread = CreateThread(NULL, 0, live_worker, NULL, 0, NULL);
}

void glass_live_follow(int x, int y, int w, int h, float blur_radius, BOOL hold)
{
    BOOL changed, radius_ok;
    float r = blur_radius * g_blur_scale;

    live_init();
    EnterCriticalSection(&g_live_lock);
    changed = (g_tx != x || g_ty != y || g_tw != w || g_th != h ||
               fabsf(g_tr - r) > 0.01f);
    g_tx = x;
    g_ty = y;
    g_tw = w;
    g_th = h;
    g_tr = r;
    radius_ok = g_ltp.valid && fabsf(g_ltp.radius - r) < 0.6f;
    LeaveCriticalSection(&g_live_lock);

    InterlockedExchange(&g_target_set, 1);
    InterlockedExchange(&g_hold, hold ? 1 : 0);

    /* The tile is bigger than the card, so most moves are served straight out
     * of it without a capture.  Only leaving the margin, or a radius change,
     * is worth waking the worker for. */
    if (changed && (!radius_ok || !tile_covers(x, y, w, h)))
    {
        InterlockedExchange(&g_force, 1);
        if (g_wake)
            SetEvent(g_wake);
    }
}

BOOL glass_live(int x, int y, int w, int h, float blur_radius, RgBitmap *out)
{
    BOOL hit = FALSE;
    float r = blur_radius * g_blur_scale;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;

    live_init();
    EnterCriticalSection(&g_live_lock);
    if (g_ltp.valid && g_ltp.px && fabsf(g_ltp.radius - r) < 0.6f)
        hit = plane_crop(&g_ltp, x, y, w, h, out);
    LeaveCriticalSection(&g_live_lock);

    if (!hit && g_wake)
    {
        InterlockedExchange(&g_force, 1);
        SetEvent(g_wake);
    }
    return hit;
}

BOOL glass_live_blocking(int x, int y, int w, int h, float blur_radius, RgBitmap *out)
{
    float r = blur_radius * g_blur_scale;
    BOOL ok;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;
    if (!tile_build(x, y, w, h, r))
        return FALSE;

    EnterCriticalSection(&g_live_lock);
    ok = plane_crop(&g_ltp, x, y, w, h, out);
    LeaveCriticalSection(&g_live_lock);
    return ok;
}

BOOL glass_capture(int x, int y, int w, int h, RgBitmap *out)
{
    BYTE *px = NULL;

    if (!out || w <= 0 || h <= 0)
        return FALSE;
    if (!capture_rect(x, y, w, h, &px))
        return FALSE;

    out->w = w;
    out->h = h;
    out->px = px;
    return TRUE;
}

BOOL glass_blurred_copy(const RgBitmap *src, float radius, RgBitmap *out)
{
    size_t bytes;
    BYTE *px;

    if (!src || !src->px || !out || src->w <= 0 || src->h <= 0)
        return FALSE;

    bytes = (size_t)src->w * src->h * 4;
    px = (BYTE *)malloc(bytes);
    if (!px)
        return FALSE;

    memcpy(px, src->px, bytes);
    blur_bgra(px, src->w, src->h, radius * g_blur_scale);

    out->w = src->w;
    out->h = src->h;
    out->px = px;
    return TRUE;
}

BOOL glass_crop(const RgBitmap *src, int w, int h, RgBitmap *out)
{
    BYTE *px;
    int row;

    if (!src || !src->px || !out || w <= 0 || h <= 0)
        return FALSE;
    if (w > src->w || h > src->h)
        return FALSE;

    px = (BYTE *)malloc((size_t)w * h * 4);
    if (!px)
        return FALSE;

    for (row = 0; row < h; row++)
        memcpy(px + (size_t)row * w * 4, src->px + (size_t)row * src->w * 4,
               (size_t)w * 4);

    out->w = w;
    out->h = h;
    out->px = px;
    return TRUE;
}

BOOL glass_live_reblur(float blur_radius)
{
    float r = blur_radius * g_blur_scale;

    live_init();
    EnterCriticalSection(&g_live_lock);
    if (g_ltp.valid && g_ltp.raw && g_ltp.radius != r)
    {
        memcpy(g_ltp.px, g_ltp.raw, (size_t)g_ltp.w * g_ltp.h * 4);
        blur_bgra(g_ltp.px, g_ltp.w, g_ltp.h, r / PLANE_SCALE);
        g_ltp.radius = r;
        InterlockedIncrement(&g_gen);
    }
    LeaveCriticalSection(&g_live_lock);
    return TRUE;
}

void glass_live_invalidate(void)
{
    live_init();
    EnterCriticalSection(&g_live_lock);
    g_ltp.valid = FALSE;
    LeaveCriticalSection(&g_live_lock);
    InterlockedExchange(&g_force, 1);
    if (g_wake)
        SetEvent(g_wake);
}

void glass_live_menu(BOOL open)
{
    InterlockedExchange(&g_menu_open, open ? 1 : 0);
    if (!open && g_wake)
        SetEvent(g_wake);
}

LONG glass_live_gen(void)
{
    return InterlockedCompareExchange(&g_gen, 0, 0);
}

BOOL glass_live_available(void)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi;
    HMODULE nt;
    RtlGetVersionFn fn;

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);

    nt = GetModuleHandleW(L"ntdll.dll");
    fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (!fn || fn(&vi) != 0)
        return FALSE;

    return vi.dwMajorVersion > 10 ||
           (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 19041);
}

/* Blur radii are authored in logical pixels, so a 125% display must blur
 * 25% further to look the same.  Without this the glass gets visibly sharper
 * (and the card looks less frosted) the higher the display scaling. */
void glass_set_scale(float scale)
{
    if (scale > 0.1f && scale < 8.0f)
    {
        g_blur_scale = scale;
        g_wpp.radius = -1.0f;
        g_ltp.radius = -1.0f;
    }
}

void glass_release(RgBitmap *b)
{
    if (!b)
        return;
    free(b->px);
    b->px = NULL;
    b->w = b->h = 0;
}

void glass_shift(RgBitmap *b, int dx, int dy)
{
    (void)b;
    (void)dx;
    (void)dy;
}

void glass_clear_cache(void)
{
    if (g_wall)
        GdipDisposeImage(g_wall);
    g_wall = NULL;
    g_wall_tried = FALSE;
    g_wall_path[0] = 0;
    g_wall_dirty = TRUE;
    g_wall_style = -1;

    live_init();
    EnterCriticalSection(&g_live_lock);
    plane_free(&g_wpp);
    plane_free(&g_ltp);
    LeaveCriticalSection(&g_live_lock);
}
