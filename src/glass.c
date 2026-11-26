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
#include "capture.h"
#include "blur_gpu.h"

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
#define LIVE_MARGIN   24      /* parked card, live switch on: grab less, grab faster */
/* A drag outruns the idle margin between two captures: the card would step
 * past the tile edge and the crop would repeat the last row/column for a
 * frame, which reads as the backdrop sticking and then snapping.  While a
 * drag owns the pacing, keep enough slack for a capture to land well before
 * the card reaches it. */
#define PLANE_MARGIN_DRAG 200
/* Pacing.  This used to be 120 ms with a 60 ms floor, i.e. ~8 fps: a video
 * playing behind the card was visibly stuttering because the backdrop only
 * caught every eighth frame.  It now runs at display rate (~60 fps) while the
 * screen is actually moving, and still backs off to a few captures a second
 * when the screen is still, so an idle desktop widget costs nothing. */
#define LIVE_IDLE_MS  6       /* + ~10 ms of capture/publish = one frame per vsync */
#define LIVE_MIN_MS   16      /* hard floor between captures: 60 fps ceiling, not 250 */
#define LIVE_STILL_MS 3       /* live fast path: >120 fps */
#define LIVE_PROBE_MS 12      /* excluded path: watchful, fast motion handoff */
#define PLATE_PROBE_MS 4      /* capture-visible: backdrop at 60 fps */
#define LIVE_DRAG_MS  16      /* drag: keep the live tile within a frame of now */
#define LIVE_SLOW_MS  400     /* drag hold: do not compete with the UI thread */

/* The idle cadence while the card is still.  This used to back off to 400 ms
 * and then 1000 ms, on the theory that a static desktop should cost almost
 * nothing.  Measured, a tile build is 0.7-1.2 ms (a plain SRCCOPY of the card
 * box; the ~6 ms figure belongs to the CAPTUREBLT variant), so the deep backoff
 * saved well under a percent of a core and bought a full second of "the
 * background lags when something starts moving" - the single most reported
 * complaint.  Looking ten times a second costs about 1 % and cuts that second
 * to a tenth.  Note this value also gates cap_set_demand(): the snapshot worker
 * must stay idle while we are only looking, or it bursts to 100 Hz. */
#define LIVE_STILL_POLL_MS 100
#define LIVE_COVERED_MS 250   /* card fully hidden: idle poll only */

/* How often the plate is re-learned from a truth grab while the card is
 * capture-visible and still.  It has to be on a clock, not on a change: the
 * pixels under the card are skipped by the change detector, so a window that
 * appears and disappears entirely behind the glass raises no request at all.
 * One second is the slowest rate that still reads as "it follows the desktop"
 * and costs about 0.3 % of one core (the wait inside plate_repair_grab() is a
 * Sleep, which burns no CPU). */
#define PLATE_REFRESH_MS 1000

typedef struct {
    BYTE *raw;      /* unblurred, at `scale` */
    BYTE *px;       /* blurred, at `scale` */
    int   w, h;     /* plane size, in plane pixels */
    int   x, y;     /* screen origin */
    float radius;
    int   scale;    /* screen px per plane px */
    BOOL  valid;
} Plane;

static Plane g_wpp;     /* wallpaper, monitor sized */
static RectI g_wpp_rect;
/* The plane holds a FROZEN SCREEN instead of the wallpaper.  Set by
 * glass_freeze_plane() for the capture-visible body: the card is in the
 * capture then, so a live grab would return the card itself, while a picture
 * taken *before* it became visible is both clean and (for a still scene)
 * indistinguishable from the real thing. */
static volatile LONG g_wpp_screen;
static Plane g_ltp;     /* live capture tile, card + margin */
static RectI g_lrect;   /* screen area the live tile covers */
static float g_blur_scale = 1.0f;
static volatile LONG g_live_idle = 1;   /* keep refreshing while the card is still */

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
    p->scale = PLANE_SCALE;
    return TRUE;
}

/* Bilinear crop of a plane into a full-resolution BGRA bitmap.  Screen
 * coordinates are clamped, so the card can hang off the plane's edge for a
 * frame without reading out of bounds. */
static BOOL plane_crop_buf(const Plane *p, const BYTE *src, int x, int y,
                           int w, int h, RgBitmap *out)
{
    BYTE *dst;
    int px, py;

    if (!p->valid || !src || w <= 0 || h <= 0 || !out)
        return FALSE;

    dst = (BYTE *)malloc((size_t)w * h * 4);
    if (!dst)
        return FALSE;

    /* Scale 1 is an integer crop: the bilinear tap collapses to the source
     * pixel itself (fx = fy = 0).  This is the zero-blur case, i.e. the sharp
     * clear-glass tile, where two full crops (blurred + rim) ran per drag
     * frame at ~1.7 ms each.  Copying rows with clamped edges produces the
     * same bytes and leaves only the edge art to the per-pixel path. */
    if (p->scale == 1)
    {
        int sx0 = x - p->x, sy0 = y - p->y;

        for (py = 0; py < h; py++)
        {
            int sy = sy0 + py;
            int left, right, i;
            const BYTE *srow;
            BYTE *drow = dst + (size_t)py * w * 4;

            if (sy < 0) sy = 0;
            if (sy > p->h - 1) sy = p->h - 1;
            srow = src + (size_t)sy * p->w * 4;

            left = sx0 < 0 ? -sx0 : 0;
            right = p->w - sx0;
            if (right > w) right = w;
            if (right < 0) right = 0;
            if (left > w) left = w;
            if (right < left) right = left;

            if (right > left)
                memcpy(drow + (size_t)left * 4,
                       srow + (size_t)(sx0 + left) * 4,
                       (size_t)(right - left) * 4);
            for (i = 0; i < left; i++)
                memcpy(drow + (size_t)i * 4, srow, 4);
            for (i = right; i < w; i++)
                memcpy(drow + (size_t)i * 4,
                       srow + (size_t)(p->w - 1) * 4, 4);
            for (i = 3; i < w * 4; i += 4)
                drow[i] = 255;
        }

        out->w = w;
        out->h = h;
        out->px = dst;
        return TRUE;
    }

    /* Aligned 2x: the crop origin sits exactly on the plane origin, so the
     * per-pixel mapping degenerates to a fixed 2x2 bilinear (tap phases are
     * always 0 or 128).  The generic path below pays two 64-bit divides per
     * pixel for the same result; this variant is what makes a frame-exact
     * drag capture fit into a display frame alongside the blur.  The plane
     * always covers the crop (pw >= (w+1)/2), so no clamping is needed. */
    if (p->scale == 2 && x == p->x && y == p->y && p->w >= (w + 1) / 2 &&
        p->h >= (h + 1) / 2)
    {
        for (py = 0; py < h; py++)
        {
            int iy = py >> 1, fy = (py & 1) ? 128 : 0;
            int iy1 = iy + 1 < p->h ? iy + 1 : iy;
            const BYTE *row0 = src + (size_t)iy * p->w * 4;
            const BYTE *row1 = src + (size_t)iy1 * p->w * 4;
            BYTE *drow = dst + (size_t)py * w * 4;

            for (px = 0; px < w; px++)
            {
                int ix = px >> 1, fx = (px & 1) ? 128 : 0;
                int ix1 = ix + 1 < p->w ? ix + 1 : ix;
                const BYTE *a = row0 + (size_t)ix * 4;
                const BYTE *b = row0 + (size_t)ix1 * 4;
                const BYTE *c = row1 + (size_t)ix * 4;
                const BYTE *d = row1 + (size_t)ix1 * 4;
                BYTE *o = drow + (size_t)px * 4;
                int k;

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

    for (py = 0; py < h; py++)
    {
        long sy = (long)(y + py - p->y) * 65536L / p->scale;
        int iy = (int)(sy >> 16);
        int fy = (int)((sy >> 8) & 0xFF);
        int iy1;
        BYTE *drow = dst + (size_t)py * w * 4;

        if (iy < 0) { iy = 0; fy = 0; }
        if (iy > p->h - 1) { iy = p->h - 1; fy = 0; }
        iy1 = iy + 1 < p->h ? iy + 1 : iy;

        for (px = 0; px < w; px++)
        {
            long sx = (long)(x + px - p->x) * 65536L / p->scale;
            int ix = (int)(sx >> 16);
            int fx = (int)((sx >> 8) & 0xFF);
            int ix1, k;
            const BYTE *a, *b, *c, *d;
            BYTE *o = drow + (size_t)px * 4;

            if (ix < 0) { ix = 0; fx = 0; }
            if (ix > p->w - 1) { ix = p->w - 1; fx = 0; }
            ix1 = ix + 1 < p->w ? ix + 1 : ix;

            a = src + ((size_t)iy * p->w + ix) * 4;
            b = src + ((size_t)iy * p->w + ix1) * 4;
            c = src + ((size_t)iy1 * p->w + ix) * 4;
            d = src + ((size_t)iy1 * p->w + ix1) * 4;

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

static BOOL plane_crop(const Plane *p, int x, int y, int w, int h, RgBitmap *out)
{
    return plane_crop_buf(p, p->px, x, y, w, h, out);
}

static BOOL plane_crop_raw(const Plane *p, int x, int y, int w, int h, RgBitmap *out)
{
    return plane_crop_buf(p, p->raw, x, y, w, h, out);
}

/* --------------------------------------------------- wallpaper plane build */

/* Rasterises the wallpaper into the wallpaper plane (half resolution) using
 * the same style rules the desktop does: fill, fit, centre, stretch, tile. */
static BOOL wp_plane_build(RectI rect, int style, int sc)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    BitmapData bd;
    UINT iw = 0, ih = 0;
    int pw = rect.w / sc, ph = rect.h / sc;

    if (!g_wall || pw <= 0 || ph <= 0)
        return FALSE;

    if (!plane_alloc(&g_wpp, pw, ph))
        return FALSE;
    g_wpp.scale = sc;

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
                dw = (float)iw / sc;
                dh = (float)ih / sc;
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
    InterlockedExchange(&g_wpp_screen, 0);   /* a wallpaper build replaces a freeze */
    return TRUE;
}

/* ----------------------------------------------------- the desktop window */

/* The card must show the DESKTOP - the wallpaper *and the icons the user put
 * on it* - and never the applications that happen to sit on top of it.
 *
 * The desktop is a window: "Progman", holding a SHELLDLL_DefView that draws
 * the icons.  Asking that window to render itself (PrintWindow with
 * PW_RENDERFULLCONTENT) returns exactly the desktop as arranged, with every
 * other top-level window left out - no capture exclusion, no reconstruction,
 * and it works no matter what is in front of the card.
 *
 * It is not cheap (a 2560x1600 desktop costs ~30 ms), so it is event driven:
 * the wallpaper's timestamp, the icon count, a monitor change or an explicit
 * invalidate trigger a rebuild, plus one slow safety pass in case an icon was
 * moved without changing the count. */
#define DESK_SAFETY_MS 20000
#define LVM_GETITEMCOUNT_C 0x1004

static HWND    g_desk_hwnd;
static HDC     g_desk_dc;
static HBITMAP g_desk_bmp;
static HGDIOBJ g_desk_old;
static void   *g_desk_bits;
static int     g_desk_w, g_desk_h;
static int     g_desk_items = -1;

static BOOL CALLBACK find_defview(HWND hwnd, LPARAM lp)
{
    if (FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL))
    {
        *(HWND *)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND desktop_window(void)
{
    HWND hwnd;

    if (g_desk_hwnd && IsWindow(g_desk_hwnd))
        return g_desk_hwnd;

    g_desk_hwnd = NULL;
    hwnd = FindWindowW(L"Progman", NULL);
    if (hwnd && FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL))
        g_desk_hwnd = hwnd;
    else
    {
        /* Wallpaper tools move the icon view into a WorkerW that owns
         * SHELLDLL_DefView; follow it instead of assuming Progman. */
        HWND found = NULL;
        EnumWindows(find_defview, (LPARAM)&found);
        g_desk_hwnd = found;
    }
    return g_desk_hwnd;
}

/* Reusable window-sized surface for the PrintWindow round trip.  Allocating a
 * full-desktop DIB on every rebuild would churn tens of megabytes. */
static BOOL desk_surface(int w, int h)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_desk_dc && g_desk_w == w && g_desk_h == h)
        return TRUE;

    if (g_desk_dc)
    {
        if (g_desk_old)
            SelectObject(g_desk_dc, g_desk_old);
        DeleteDC(g_desk_dc);
        g_desk_dc = NULL;
        g_desk_old = NULL;
    }
    if (g_desk_bmp)
    {
        DeleteObject(g_desk_bmp);
        g_desk_bmp = NULL;
    }
    g_desk_bits = NULL;
    g_desk_w = g_desk_h = 0;

    screen = GetDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_desk_dc = CreateCompatibleDC(screen);
    g_desk_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_desk_bits, NULL, 0);
    ReleaseDC(NULL, screen);

    if (!g_desk_dc || !g_desk_bmp || !g_desk_bits)
        return FALSE;

    g_desk_old = SelectObject(g_desk_dc, g_desk_bmp);
    g_desk_w = w;
    g_desk_h = h;
    return TRUE;
}

/* Cheap probe: adding or removing a desktop icon changes this count, and that
 * is by far the most common way the desktop changes during a session. */
static BOOL desk_items_changed(void)
{
    HWND def = FindWindowExW(desktop_window(), NULL, L"SHELLDLL_DefView", NULL);
    HWND lv = def ? FindWindowExW(def, NULL, L"SysListView32", NULL) : NULL;
    LRESULT n = -1;

    if (lv)
    {
        /* SendMessageTimeout, not SendMessage: a busy Explorer must never be
         * able to stall our UI thread. */
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(lv, LVM_GETITEMCOUNT_C, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &r))
            n = (LRESULT)r;
    }

    if ((int)n != g_desk_items)
    {
        g_desk_items = (int)n;
        return TRUE;
    }
    return FALSE;
}

static void desk_surface_free(void)
{
    if (g_desk_dc)
    {
        if (g_desk_old)
        {
            SelectObject(g_desk_dc, g_desk_old);
            g_desk_old = NULL;
        }
        if (g_desk_bmp)
        {
            DeleteObject(g_desk_bmp);
            g_desk_bmp = NULL;
        }
        DeleteDC(g_desk_dc);
        g_desk_dc = NULL;
    }
    g_desk_bits = NULL;
    g_desk_w = g_desk_h = 0;
}

static BOOL desk_plane_build(RectI rect, int scale)
{
    HWND hwnd = desktop_window();
    RECT wr;
    int pw = rect.w / scale, ph = rect.h / scale;
    BITMAPINFO bi;
    HDC dst;
    HBITMAP bmp;
    HGDIOBJ old;
    void *bits = NULL;
    BOOL ok = FALSE;

    if (!hwnd || !GetWindowRect(hwnd, &wr))
        return FALSE;
    if (pw <= 0 || ph <= 0)
        return FALSE;
    if (!desk_surface(wr.right - wr.left, wr.bottom - wr.top))
        return FALSE;

    if (!PrintWindow(hwnd, g_desk_dc, 2 /* PW_RENDERFULLCONTENT */))
        return FALSE;

    /* Some drivers hand back an all-black desktop (composition off, secure
     * desktop).  Treat that as a miss so the caller can fall back. */
    {
        const DWORD *q = (const DWORD *)g_desk_bits;
        size_t n = (size_t)g_desk_w * (size_t)g_desk_h / 16, i, lit = 0;
        for (i = 0; i < n; i++)
            if ((q[i * 16] & 0x00FFFFFFu) > 0x080808u)
                lit++;
        if (lit < n / 40)
            return FALSE;
    }

    if (!plane_alloc(&g_wpp, pw, ph))
        return FALSE;
    g_wpp.scale = scale;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dst = CreateCompatibleDC(g_desk_dc);
    bmp = dst ? CreateDIBSection(g_desk_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!dst || !bmp || !bits)
    {
        if (bmp) DeleteObject(bmp);
        if (dst) DeleteDC(dst);
        plane_free(&g_wpp);
        return FALSE;
    }

    old = SelectObject(dst, bmp);
    SetStretchBltMode(dst, HALFTONE);
    SetBrushOrgEx(dst, 0, 0, NULL);
    ok = StretchBlt(dst, 0, 0, pw, ph, g_desk_dc,
                    rect.x - wr.left, rect.y - wr.top,
                    rect.w, rect.h, SRCCOPY) != FALSE;
    GdiFlush();
    if (ok)
        memcpy(g_wpp.raw, bits, (size_t)pw * (size_t)ph * 4);

    SelectObject(dst, old);
    DeleteObject(bmp);
    DeleteDC(dst);

    if (!ok)
    {
        plane_free(&g_wpp);
        return FALSE;
    }

    g_wpp.x = rect.x;
    g_wpp.y = rect.y;
    g_wpp_rect = rect;
    g_wpp.valid = TRUE;
    InterlockedExchange(&g_wpp_screen, 0);   /* a desktop build replaces a freeze */
    desk_surface_free();   /* steady state keeps the plane only */
    return TRUE;
}

/* The wallpaper only changes when the user changes it, so probing the file
 * and the registry on every frame is pure waste - and this runs on every
 * frame of a drag.  Two seconds is short enough to notice a change and long
 * enough that a drag never pays for it. */
#define WP_RECHECK_MS 2000

static BOOL wp_plane_ready(float radius, int x, int y)
{
    static ULONGLONG checked_at, built_at;
    ULONGLONG now = GetTickCount64();
    RectI rect;
    BOOL recheck = !g_wpp.valid || now - checked_at > WP_RECHECK_MS;
    BOOL items_dirty = FALSE;

    if (recheck)
    {
        checked_at = now;
        /* The wallpaper FILE is only needed when the PrintWindow grab fails;
         * decoding it here held ~16 MB of GDI+ bitmap alive for nothing. */
        g_wall_style = reg_read_int(L"Control Panel\\Desktop", L"WallpaperStyle", 10);
        /* Cross-process SendMessage: only worth paying on the 2 s tick, not on
         * every frame the card repaints. */
        items_dirty = desk_items_changed();
    }

    /* The plane is built per monitor, so moving the card to a second display
     * rebuilds it instead of sampling the wrong wallpaper. */
    rect = (g_wall_style == 22) ? virtual_rect() : monitor_rect_at(x, y);

    /* Same resolution rule as the live tile path: a clear glass (blur 0)
     * samples the desktop 1:1 - the old fixed half-res plane upsampled 2x
     * and read noticeably softer than the live screen grab.  With blur on,
     * half-res is indistinguishable (the blur dominates) and saves the
     * second full-monitor buffer. */
    {
        int scale = radius < 0.75f ? 1 : PLANE_SCALE;
        BOOL frozen = InterlockedCompareExchange(&g_wpp_screen, 0, 0) != 0;

        /* A frozen screen is a photograph: there is nothing to re-read, and it
         * must never be silently replaced by the wallpaper. */
        if (frozen)
        {
            if (!g_wpp.valid || !g_wpp.raw)
                return FALSE;
        }
        else if (!g_wpp.valid || g_wall_dirty || items_dirty ||
                 (InterlockedCompareExchange(&g_live_idle, 0, 0) &&
                  now - built_at > DESK_SAFETY_MS) ||
                 g_wpp.scale != scale ||
                 g_wpp_rect.w != rect.w || g_wpp_rect.h != rect.h ||
                 g_wpp_rect.x != rect.x || g_wpp_rect.y != rect.y)
        {
            BOOL built = desk_plane_build(rect, scale);

            if (!built)
            {
                load_wallpaper();
                if (g_wall)
                    built = wp_plane_build(rect, g_wall_style, scale);
            }
            if (!built)
                return FALSE;

            built_at = now;
            g_wall_dirty = FALSE;
            items_dirty = FALSE;
            g_wpp.radius = -1.0f;
        }

        if (g_wpp.radius != radius && g_wpp.raw)
        {
            if (radius < 0.6f)
            {
                /* Clear glass: the crop reads raw directly, so the second
                 * (blurred) buffer is pure waste - keep it unallocated. */
                free(g_wpp.px);
                g_wpp.px = NULL;
            }
            else
            {
                if (!g_wpp.px)
                    g_wpp.px = (BYTE *)malloc((size_t)g_wpp.w * g_wpp.h * 4);
                if (!g_wpp.px)
                    return FALSE;
                memcpy(g_wpp.px, g_wpp.raw, (size_t)g_wpp.w * g_wpp.h * 4);
                /* Blur in plane pixels: a frozen plane keeps its own scale
                 * even when the current radius would have picked another. */
                blur_bgra(g_wpp.px, g_wpp.w, g_wpp.h,
                          radius / (float)(g_wpp.scale ? g_wpp.scale : scale));
            }
            g_wpp.radius = radius;
        }
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

    return g_wpp.px ? plane_crop(&g_wpp, x, y, w, h, out)
                    : plane_crop_raw(&g_wpp, x, y, w, h, out);
}

BOOL glass_backdrop_rim(int x, int y, int w, int h, RgBitmap *out)
{
    if (w <= 0 || h <= 0 || !out)
        return FALSE;
    return plane_crop_raw(&g_wpp, x, y, w, h, out);
}

/* ------------------------------------------------------------ live capture */

static CRITICAL_SECTION g_live_lock;
static volatile LONG    g_gen;
static volatile LONG    g_quit;
static volatile LONG    g_menu_open;
static volatile LONG    g_target_set;
static volatile LONG    g_force;
static volatile LONG    g_hold;      /* a drag owns the frame pacing */
static volatile LONG    g_suspend;   /* session locked: no grabs at all */
static HANDLE           g_wake;
static HANDLE           g_thread;
static HWND             g_notify;
static int              g_tx, g_ty, g_tw, g_th;
static float            g_tr;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;

static int live_margin(void)
{
    /* A parked card does not travel, and the backdrop is rebuilt ~60 times a
     * second: the grab box IS the readback cost, so keep the slack tight.
     * Anything that actually moves the card goes through g_hold / the drag
     * path, which uses its own deep margin. */
    return InterlockedCompareExchange(&g_hold, 0, 0) ? PLANE_MARGIN_DRAG
                                                     : LIVE_MARGIN;
}

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

/* --- removing our own layered windows from our own grabs -----------------
 * "Visible in screenshots" deliberately leaves both layered windows in the
 * DWM desktop surface, so a plain BitBlt of that surface contains the widget
 * and the menu.  Toggling WDA_EXCLUDEFROMCAPTURE around every BitBlt (what
 * this build did before) works for the grab, but external recorders can
 * sample the window while that flag is on: the card visibly blinks in their
 * video.  The affinity is now set once for the user's choice and never
 * touched during capture.
 *
 * Instead we keep the last uploaded layered surface for both windows and,
 * after the grab, reconstruct the desktop under their footprint.  Undoing
 * DWM's SourceOver is only useful for the faint antialiasing tail (alpha
 * below 4); everything else is a hole: the card paints its glass from a
 * pre-blurred crop, so its rim and body colours are themselves backdrop,
 * and the opaque core is not recoverable arithmetically at all.  The hole is
 * filled by diffusing the untouched pixels around it inwards, which is what
 * a stronger blur would show anyway.
 *
 * Keeping the surface also tells the grab where the windows are; the frame
 * section below makes the grab wait for exactly the upload it remembers.
 *
 * If a surface has not been uploaded yet, the old low-pass reconstruction
 * below still provides a fallback for that frame. */
static CRITICAL_SECTION g_cap_lock;
static INIT_ONCE        g_cap_once = INIT_ONCE_STATIC_INIT;
static volatile BOOL    g_capture_want = TRUE;     /* what the user picked */
/* volatile: written on the UI thread (live_fast_sync) and read on the capture
 * worker (capture_rect, tile_build).  It was a plain BOOL, so the worker's
 * read had no guarantee of ever seeing an update. */
static volatile BOOL    g_capture_visible = TRUE;  /* effective (see live_fast_sync) */
static volatile LONG    g_live_fast;               /* live bg active: card excluded */
static volatile LONG    g_seen_motion;             /* tick of the last visible change */
static volatile LONG    g_diff_run;                /* consecutive frames that differed */
static volatile LONG    g_fast_hold;               /* tick until which fast mode is kept */
static volatile LONG    g_fast_since;              /* when exclusion was last turned on */
static volatile BOOL     g_live_fast_on;           /* applied state of the above */
static BOOL             g_live_hires;              /* timeBeginPeriod(1) held */

typedef struct {
    BYTE *px;
    int   cap, w, h, x, y;
    BOOL  valid;
} SelfLayer;

static SelfLayer g_self[2];

/* The startup seed runs while the window is created but still hidden, so the
 * grab is already clean even though a surface is registered.  Ignoring the
 * layers for that one capture is what lets the plate learn the pixels the
 * card is about to cover. */
static BOOL g_seed_mode;

static RECT g_menu_rect;
static BOOL g_menu_rect_valid;

static BOOL CALLBACK cap_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cap_lock);
    return TRUE;
}

/* A screen grab must run in the same critical section as the upload whose
 * footprint it reconstructs.  Otherwise a drag moves the card between
 * "surface remembered" and "pixels composed", the hole is cut where the card
 * no longer is, and the backdrop gets a ghost of the widget that the rim
 * then samples back as coloured streaks. */
void glass_frame_begin(void)
{
    InitOnceExecuteOnce(&g_cap_once, cap_once, NULL, NULL);
    EnterCriticalSection(&g_cap_lock);
}

void glass_frame_end(void)
{
    LeaveCriticalSection(&g_cap_lock);
}

typedef HRESULT (WINAPI *PFN_DwmFlush)(void);

void glass_present(void)
{
    static PFN_DwmFlush pfn;
    static BOOL tried;

    if (!tried)
    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        tried = TRUE;
        if (dwm)
            *(FARPROC *)&pfn = GetProcAddress(dwm, "DwmFlush");
    }
    if (pfn)
        pfn();
}

/* which: 0 = card, 1 = context menu.  Pass NULL to forget the surface. */
static volatile LONG g_self_tick;   /* when the card surface last changed */

/* Age of the card's surface in ms.  A grab may only be taken with the fast
 * duplication path while the surface has been stable for longer than one
 * composition: otherwise the frame can still contain the previous upload, and
 * inverting the composite against the new one leaves a ghost of the old. */
int glass_self_age_ms(void)
{
    LONG t0 = InterlockedCompareExchange(&g_self_tick, 0, 0);
    if (!t0)
        return 1 << 20;
    return (int)((DWORD)GetTickCount() - (DWORD)t0);
}

void glass_set_self_layer(int which, const BYTE *px, int w, int h, int x, int y)
{
    SelfLayer *L;
    size_t bytes;

    if (which < 0 || which > 1)
        return;
    InitOnceExecuteOnce(&g_cap_once, cap_once, NULL, NULL);
    EnterCriticalSection(&g_cap_lock);
    L = &g_self[which];
    if (!px || w <= 0 || h <= 0)
    {
        L->valid = FALSE;
        LeaveCriticalSection(&g_cap_lock);
        return;
    }
    bytes = (size_t)w * h * 4;
    if (bytes > (size_t)L->cap)
    {
        BYTE *nb = (BYTE *)realloc(L->px, bytes);
        if (!nb)
        {
            L->valid = FALSE;
            LeaveCriticalSection(&g_cap_lock);
            return;
        }
        L->px = nb;
        L->cap = (int)bytes;
    }
    memcpy(L->px, px, bytes);
    if (which == 0)
        InterlockedExchange(&g_self_tick, (LONG)GetTickCount());
    L->w = w;
    L->h = h;
    L->x = x;
    L->y = y;
    L->valid = TRUE;
    LeaveCriticalSection(&g_cap_lock);
}

/* TRUE when a layered window covers this screen pixel with anything but its
 * faintest antialiasing.  Every such pixel has to go: the card paints its
 * "glass" from a pre-blurred backdrop, so even its semi-transparent rim and
 * shadow colours are partly baked backdrop.  Subtracting them would amplify
 * the difference against the real desktop (the rainbow fringes), and the
 * opaque core cannot be subtracted at all. */

/* --- windows in FRONT of the card ---------------------------------------
 *
 * Pixels another window covers belong to that window, not to the backdrop the
 * glass samples.  Learning them (and counting them as a scene change) made a
 * window sliding over the card rewrite the plate: the invalidator fired, the
 * sharp snapshot under the card was dropped, and the still-visible remainder
 * of the glass turned into a flat diffusion until the card was moved.  Rects
 * of the windows above the card are masked out of learning and of the change
 * test instead. */
#define OCCL_MAX 24


/* Record/HUD overlays are layered with a low alpha: they own what they paint,
 * but they are too faint to be treated as a hard cover either. */

/* Rects of the visible windows above `card` that touch the capture area. */

/* Fill the pixels our opaque parts made unrecoverable, from their neighbours.
 * They are text, icon and rim shapes a few pixels wide, and the tile is about
 * to be blurred, so nearest-ring averaging is indistinguishable from a proper
 * inpaint here. */
static void unblend_fill(BYTE *px, int cw, int ch, BYTE *hole)
{
    int x, y;

    for (y = 0; y < ch; y++)
    {
        for (x = 0; x < cw; x++)
        {
            BYTE *d;
            int k, n = 0, sr = 0, sg = 0, sb = 0;

            if (!hole[(size_t)y * cw + x])
                continue;
            d = px + ((size_t)y * cw + x) * 4;

            for (k = 1; k <= 24 && n < 2; k++)
            {
                static const int dx[4] = { -1, 1, 0, 0 };
                static const int dy[4] = { 0, 0, -1, 1 };
                int j;

                for (j = 0; j < 4; j++)
                {
                    int xx = x + dx[j] * k, yy = y + dy[j] * k;
                    const BYTE *s;

                    if (xx < 0 || yy < 0 || xx >= cw || yy >= ch)
                        continue;
                    if (hole[(size_t)yy * cw + xx])
                        continue;
                    s = px + ((size_t)yy * cw + xx) * 4;
                    sb += s[0];
                    sg += s[1];
                    sr += s[2];
                    n++;
                }
            }
            if (n)
            {
                d[0] = (BYTE)(sb / n);
                d[1] = (BYTE)(sg / n);
                d[2] = (BYTE)(sr / n);
                hole[(size_t)y * cw + x] = 2;   /* filled */
            }
            d[3] = 255;
        }
    }

    /* The ring search only reaches 24 px, so a large fully-opaque area (an
     * opaque MENU surface, or a card rendered without the translucency cap)
     * keeps its own picture in the middle.  That residue is precisely the
     * "the glass shows a blurred copy of itself" artefact.  Finish the job in
     * two linear sweeps: interpolate along each row between the nearest known
     * pixels, then do the same down each column for rows that had none.  A
     * linear ramp is invisible after the blur that follows. */
    for (y = 0; y < ch; y++)
    {
        int last = -1;
        int k;
        for (x = 0; x < cw; x++)
        {
            size_t o = (size_t)y * cw + x;
            if (hole[o] == 2)
                last = x;
            else if (hole[o] == 1 && last >= 0)
            {
                int nx = -1;
                for (k = x + 1; k < cw; k++)
                {
                    if (hole[(size_t)y * cw + k] != 1)
                    {
                        nx = k;
                        break;
                    }
                }
                {
                    BYTE *d = px + o * 4;
                    const BYTE *a1 = px + ((size_t)y * cw + last) * 4;
                    const BYTE *a2 = nx >= 0 ? px + ((size_t)y * cw + nx) * 4 : a1;
                    int t = (nx > last) ? (x - last) * 256 / (nx - last) : 0;

                    d[0] = (BYTE)((a1[0] * (256 - t) + a2[0] * t) >> 8);
                    d[1] = (BYTE)((a1[1] * (256 - t) + a2[1] * t) >> 8);
                    d[2] = (BYTE)((a1[2] * (256 - t) + a2[2] * t) >> 8);
                    d[3] = 255;
                    hole[o] = 2;
                }
            }
        }
    }

    for (x = 0; x < cw; x++)
    {
        int last = -1;
        int k;
        for (y = 0; y < ch; y++)
        {
            size_t o = (size_t)y * cw + x;
            if (hole[o] == 2)
                last = y;
            else if (hole[o] == 1 && last >= 0)
            {
                int ny = -1;
                for (k = y + 1; k < ch; k++)
                {
                    if (hole[(size_t)k * cw + x] != 1)
                    {
                        ny = k;
                        break;
                    }
                }
                {
                    BYTE *d = px + o * 4;
                    const BYTE *a1 = px + ((size_t)last * cw + x) * 4;
                    const BYTE *a2 = ny >= 0 ? px + ((size_t)ny * cw + x) * 4 : a1;
                    int t = (ny > last) ? (y - last) * 256 / (ny - last) : 0;

                    d[0] = (BYTE)((a1[0] * (256 - t) + a2[0] * t) >> 8);
                    d[1] = (BYTE)((a1[1] * (256 - t) + a2[1] * t) >> 8);
                    d[2] = (BYTE)((a1[2] * (256 - t) + a2[2] * t) >> 8);
                    d[3] = 255;
                    hole[o] = 2;
                }
            }
        }
    }

    /* Anything still empty (a column with no known pixel at all) gets the
     * whole-rect average; it is a blurred backdrop, a flat patch is honest. */
    {
        int still = 0, sr = 0, sg = 0, sb = 0, n = 0;

        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++)
            {
                size_t o = (size_t)y * cw + x;
                if (hole[o] == 1)
                    still = 1;
                else if (hole[o] != 0)
                {
                    const BYTE *t = px + o * 4;
                    sb += t[0]; sg += t[1]; sr += t[2]; n++;
                }
            }
        if (still && n)
        {
            for (y = 0; y < ch; y++)
                for (x = 0; x < cw; x++)
                {
                    size_t o = (size_t)y * cw + x;
                    if (hole[o] == 1)
                    {
                        BYTE *d = px + o * 4;
                        d[0] = (BYTE)(sb / n);
                        d[1] = (BYTE)(sg / n);
                        d[2] = (BYTE)(sr / n);
                        d[3] = 255;
                    }
                }
        }
    }
}

/* Solve the compositor equation backwards.
 *
 * A card that is IN the capture is composited by DWM, so a screen grab contains
 * it.  But the composite is only
 *
 *     c = bg * (1 - a) + f
 *
 * and both `a` and `f` are known exactly - they are the premultiplied surface
 * this process uploaded.  So the background can simply be solved for:
 *
 *     bg = (c - f) / (1 - a)
 *
 * That is exact to 8-bit rounding wherever the card is translucent, which it is
 * by design (body alpha ~0.9 at full opacity).  Measured against a ground-truth
 * grab of the same rect with the card moved away: p95 error 12 levels at
 * a = 0.91, and the tile is blurred by 8-26 px straight after, which erases it.
 *
 * Where the surface is opaque the equation has no solution, but those pixels
 * are exactly the ones the card paints over (glyphs, icon, rim), so they are
 * marked and filled from their neighbours.  Nothing of ours survives - and the
 * card never has to leave the capture to see what is behind it. */
static int g_unblend_inv[256];   /* 255 * 65536 / (255 - a), lazily built */

static BOOL self_unblend(BYTE *px, int cw, int ch, int ox, int oy)
{
    static BYTE *hole;
    static int hole_cap;
    size_t need = (size_t)cw * (size_t)ch;
    int i;
    BOOL touched = FALSE;

    if (!px || cw <= 0 || ch <= 0)
        return FALSE;
    if (!g_unblend_inv[1])
    {
        int a;
        for (a = 0; a < 247; a++)
            g_unblend_inv[a] = (int)((255 * 65536) / (255 - a));
    }
    if ((int)need > hole_cap)
    {
        BYTE *nb = (BYTE *)realloc(hole, need);
        if (!nb)
            return FALSE;
        hole = nb;
        hole_cap = (int)need;
    }
    memset(hole, 0, need);

    for (i = 0; i < 2; i++)
    {
        const SelfLayer *L = &g_self[i];
        RECT lr, hit;
        int x, y;

        if (!L->valid || !L->px)
            continue;
        lr.left = L->x; lr.top = L->y;
        lr.right = L->x + L->w; lr.bottom = L->y + L->h;
        if (!IntersectRect(&hit, &lr, &(RECT){ ox, oy, ox + cw, oy + ch }))
            continue;

        touched = TRUE;
        for (y = hit.top; y < hit.bottom; y++)
        {
            for (x = hit.left; x < hit.right; x++)
            {
                size_t loff = ((size_t)(y - lr.top) * (size_t)L->w +
                               (size_t)(x - lr.left)) * 4;
                size_t poff = (size_t)(y - oy) * (size_t)cw + (size_t)(x - ox);
                BYTE *d = px + poff * 4;
                int a = L->px[loff + 3];

                if (a == 0)
                    continue;
                if (a >= 247)
                {
                    /* Opaque: the background is not in this pixel at all. */
                    hole[poff] = 1;
                    continue;
                }
                /* bg = (c - f) * 255 / (255 - a), f already premultiplied.
                 * The reciprocal is a table lookup: this runs over every pixel
                 * of the card on every drag frame, and an integer divide there
                 * cost a measurable slice of the frame. */
                {
                    int c0 = d[0], c1 = d[1], c2 = d[2];
                    int f0 = L->px[loff + 0], f1 = L->px[loff + 1];
                    int f2 = L->px[loff + 2];
                    int k = g_unblend_inv[a];

                    d[0] = (BYTE)rg_clampi(((c0 - f0) * k) >> 16, 0, 255);
                    d[1] = (BYTE)rg_clampi(((c1 - f1) * k) >> 16, 0, 255);
                    d[2] = (BYTE)rg_clampi(((c2 - f2) * k) >> 16, 0, 255);
                    d[3] = 255;
                }
            }
        }
    }

    if (!touched)
        return FALSE;
    unblend_fill(px, cw, ch, hole);
    return TRUE;
}


/* Last-resort reconstruction for a frame whose layered surface is not known
 * yet (startup, or the OS refused the upload). */
static void punch_capture(BYTE *px, int cw, int ch, int ox, int oy, int smooth);
static void self_punch_light(BYTE *px, int cw, int ch, int ox, int oy);

/* Grabs a screen rectangle at full resolution.
 *
 * DXGI Desktop Duplication first: the frame is a GPU-side copy of the
 * composed desktop taken by a background thread, and copying the wanted rect
 * out of it costs 0.2-0.8 ms instead of the ~3.5 ms a GDI BitBlt readback
 * stalls for on hybrid graphics.  With WDA_EXCLUDEFROMCAPTURE the frames
 * never contain the widget, so the pixels are used as-is; a capture-visible
 * card is un-composited out of them from the plate.  Any rect the duplication
 * cannot serve (virtual displays, an open handoff) falls back to GDI. */
/* Debug only (RG_DEBUG_GRAB=1): write the raw grab after un-blending so a
 * capture problem can be told apart from a rendering one. */
static void debug_write_raw(const BYTE *px, int w, int h, const char *tag)
{
    static int want = -1;
    char path[MAX_PATH];
    HANDLE f;
    DWORD wrote = 0;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    size_t bytes;

    if (want < 0)
        want = GetEnvironmentVariableA("RG_DEBUG_GRAB", NULL, 0) > 0 ? 1 : 0;
    if (!want || !px)
        return;
    GetTempPathA(MAX_PATH, path);
    strcat(path, tag);
    bytes = (size_t)w * h * 4;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = (DWORD)(fh.bfOffBits + bytes);
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)bytes;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, &fh, sizeof(fh), &wrote, NULL);
    WriteFile(f, &ih, sizeof(ih), &wrote, NULL);
    WriteFile(f, px, (DWORD)bytes, &wrote, NULL);
    CloseHandle(f);
}

static BOOL capture_rect(int x, int y, int w, int h, BYTE **out_px)
{
    BITMAPINFO bi;
    HDC screen, mem;
    HBITMAP dib;
    HGDIOBJ old;
    void *bits = NULL;
    BYTE *px = NULL;
    BOOL ok = FALSE;

    BOOL widget_in_frame = g_capture_visible && !g_seed_mode;

    if (w <= 0 || h <= 0 || !out_px)
        return FALSE;

    px = (BYTE *)malloc((size_t)w * h * 4);
    if (!px)
        return FALSE;

    /* DXGI first when the frame cannot contain the widget: WDA-excluded or
     * hidden (seed).  A capture-visible card stays on the synchronous GDI
     * grab below, whose composition always matches the registered self
     * layer the plate is about to un-composite - a cached DXGI frame may
     * predate the last upload and cut the hole where the card no longer is. */
    if (!widget_in_frame && cap_grab(x, y, w, h, px))
    {
        *out_px = px;
        return TRUE;
    }

    /* A drag owns the visuals through the UI thread's frame-exact grab; a
     * tile build here only keeps the worker's tile warm.  It must never hold
     * g_cap_lock across a grab plus the plate-learning self_erase(): that is
     * the lock the UI thread waits on to upload drag frames, and holding it
     * is what produced the one ~40 ms hitch at every drag start. */
    if (InterlockedCompareExchange(&g_hold, 0, 0))
    {
        HDC scr = GetDC(NULL), mem2 = scr ? CreateCompatibleDC(scr) : NULL;
        HBITMAP dib2 = NULL;
        void *bits2 = NULL;

        if (mem2)
        {
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dib2 = CreateDIBSection(mem2, &bi, DIB_RGB_COLORS, &bits2, NULL, 0);
        }
        if (dib2 && bits2)
        {
            HGDIOBJ old2 = SelectObject(mem2, dib2);
            BOOL blt2 = BitBlt(mem2, 0, 0, w, h, scr, x, y, SRCCOPY);
            if (!blt2)
                blt2 = BitBlt(mem2, 0, 0, w, h, scr, x, y, SRCCOPY | CAPTUREBLT);
            if (blt2)
            {
                memcpy(px, bits2, (size_t)w * h * 4);
                if (widget_in_frame &&
                    !self_unblend(px, w, h, x, y))
                    self_punch_light(px, w, h, x, y);
                *out_px = px;
                ok = TRUE;
            }
            SelectObject(mem2, old2);
            DeleteObject(dib2);
        }
        if (mem2)
            DeleteDC(mem2);
        if (scr)
            ReleaseDC(NULL, scr);
        if (!ok)
            free(px);
        return ok;
    }

    /* No DwmFlush here.  The upload and its self layer are published under
     * g_cap_lock, and a still card keeps both at the same position, so a grab
     * one composition old reconstructs the same hole from the plate.  On an
     * idle desktop DwmFlush is documented here to stall for hundreds of
     * milliseconds; paying that on every capture-visible tile was the delay
     * users saw after turning "visible in screenshots" on. */

    InitOnceExecuteOnce(&g_cap_once, cap_once, NULL, NULL);
    EnterCriticalSection(&g_cap_lock);

    screen = GetDC(NULL);
    if (!screen)
        goto done;
    mem = CreateCompatibleDC(screen);
    if (!mem)
    {
        ReleaseDC(NULL, screen);
        goto done;
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
        /* CAPTUREBLT asks the compositor for a fresh frame, which costs ~6 ms -
         * a third of the 16.7 ms budget for 60 fps.  A plain SRCCOPY grab is
         * ~10x cheaper and is enough for ordinary windows; it is only tried
         * first, with the flagged grab as a fallback. */
        /* CAPTUREBLT first: a plain SRCCOPY read of the desktop returns the
         * compositor's *cached* copy of window content, so a video, a scrolling
         * page or a moving window behind the card came back identical frame
         * after frame - the tile then decided "nothing changed" and the glass
         * looked frozen at ~1 fps.  The flagged grab asks for a fresh
         * composition; it costs a couple of milliseconds more and is the whole
         * difference between a live backdrop and a photograph. */
        BOOL blt = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

        if (!blt)
            blt = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

        if (blt)
        {
            memcpy(px, bits, (size_t)w * h * 4);
            /* With WDA_EXCLUDEFROMCAPTURE the widget is not in the grab at
             * all; when it is capture-visible, reconstruct what it covers. */
            /* Un-composite, do NOT replay a remembered plate: the plate can
             * only ever hold what was visible before the card covered it, so
             * anything behind the card - a video, a scrolling page, another
             * window - stays frozen forever and every frame compares equal
             * (that is why the backdrop looked like 1 fps).  Solving the
             * compositor equation gives the real pixels every frame. */
            if (widget_in_frame &&
                !self_unblend(px, w, h, x, y))
                punch_capture(px, w, h, x, y, 0);
            debug_write_raw(px, w, h, "rg_grab.bmp");
            *out_px = px;
            ok = TRUE;
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }

    DeleteDC(mem);
    ReleaseDC(NULL, screen);

done:
    LeaveCriticalSection(&g_cap_lock);
    if (!ok)
        free(px);
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
static volatile LONG g_live_frames;   /* tiles published since start */
static volatile LONG g_cap_us, g_build_us, g_builds;

LONG glass_live_frames(void)
{
    return InterlockedCompareExchange(&g_live_frames, 0, 0);
}

/* Average microseconds of a tile rebuild, and the capture part of it.  Used by
 * the RG_PROF=1 frame-rate log to see where the time goes. */
void glass_live_timings(LONG *cap_us, LONG *build_us, LONG *builds)
{
    LONG n = InterlockedCompareExchange(&g_builds, 0, 0);
    if (cap_us)   *cap_us   = n ? InterlockedCompareExchange(&g_cap_us, 0, 0) / n : 0;
    if (build_us) *build_us = n ? InterlockedCompareExchange(&g_build_us, 0, 0) / n : 0;
    if (builds)   *builds   = n;
    InterlockedExchange(&g_cap_us, 0);
    InterlockedExchange(&g_build_us, 0);
    InterlockedExchange(&g_builds, 0);
}

static volatile LONG g_plane_tick;
/* Freshness of the frame-exact drag capture (glass_drag_backdrop).  The
 * painted backdrop during a drag comes from that path, so age accounting
 * must see it or every drag frame would report the worker tile's latency. */
static volatile LONG g_sync_tick;

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
        g_lrect.w = g_ltp.w * g_ltp.scale;
        g_lrect.h = g_ltp.h * g_ltp.scale;
        InterlockedExchange(&g_plane_tick, (LONG)GetTickCount64());
    }
    InterlockedIncrement(&g_gen);
    InterlockedIncrement(&g_live_frames);
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

/* --- fallback reconstruction for capture-visible mode -------------------
 *
 * self_erase() normally removes our windows exactly, from their uploaded
 * surfaces.  This punch path is only used for a frame where a surface is not
 * known yet: it fills the remembered rectangle with the surrounding desktop.
 * The blur that follows hides the interpolation. */
static RECT g_widget_rect;
static BOOL g_widget_rect_valid;

/* --- live background fast path ------------------------------------------
 *
 * "Live background" means the glass body has to follow the desktop the card
 * covers on EVERY frame.  In capture-visible mode that is impossible at any
 * useful rate: the grab contains our own surface there, so the pixels under
 * the card only exist in the plate, and the plate needs a truth grab that
 * hides the card for ~35 ms of compositor sync - 4 Hz, i.e. the slideshow the
 * switch produced.
 *
 * The only way to get those pixels per frame is to keep the card out of capture
 * for as long as the switch is on.  The affinity is therefore set once per
 * toggle (never per frame, so nothing blinks in a recording): while the switch
 * is on the card does not appear in screenshots, and capture_rect() reads the
 * real desktop straight from the capture snapshot at full rate.
 *
 * Turning the switch off restores "visible in screenshots", but the plate has
 * learned nothing during the live session - so learn the truth first, while the
 * card is still excluded, or the glass would come back frozen on the pixels
 * from before the switch. */
/* The fast path costs the user something real: an excluded card does not
 * appear in screenshots or recordings.  It is therefore only taken while
 * something behind the card is actually moving - that is the only time the
 * "live" pixels matter.  A still desktop goes back to the capture-visible plate
 * path (and to the card being recordable) after a short settle. */
static BOOL live_motion_fast(void)
{
    static DWORD born;
    DWORD now = GetTickCount();
    DWORD seen = (DWORD)InterlockedCompareExchange(&g_seen_motion, 0, 0);
    DWORD hold = (DWORD)InterlockedCompareExchange(&g_fast_hold, 0, 0);

    /* The first frames after start differ by definition (nothing published
     * yet).  Do not read that as "the desktop is playing a video". */
    if (!born)
        born = now ? now : 1;
    if ((DWORD)(now - born) < 1500)
        return FALSE;

    if ((LONG)(hold - now) > 0)
        return TRUE;
    return seen && (DWORD)(now - seen) < 2000;
}

static void live_fast_sync(void)
{
    BOOL base = (InterlockedCompareExchange(&g_live_idle, 0, 0) != 0) &&
                (InterlockedCompareExchange(&g_target_set, 0, 0) != 0);
    /* The fast path exists to get a clean grab: it keeps the card out of the
     * capture.  That is the opposite of what a user who ticked "visible in
     * screenshots" asked for, so it must not engage for them - otherwise every
     * burst of motion silently drops the card out of their screenshots for two
     * seconds afterwards, which reads as the switch being broken. */
    BOOL want = base && live_motion_fast() && !g_capture_want;
    BOOL vis = want ? FALSE : g_capture_want;

    if (want && !g_live_fast_on)
        InterlockedExchange(&g_fast_since, (LONG)GetTickCount());
    g_live_fast_on = want;
    InterlockedExchange(&g_live_fast, want ? 1 : 0);
    g_capture_visible = vis;

    if (g_notify && IsWindow(g_notify))
        SetWindowDisplayAffinity(g_notify, vis ? 0 : 0x00000011);

    /* Hold the mode for a few seconds so a scene that flickers in and out of
     * motion cannot flap the affinity.  Turning the exclusion ON needs one
     * composition before the capture path stops containing our own surface;
     * tile_build() enforces that delay on the worker, never on this thread. */
    if (want)
        InterlockedExchange(&g_fast_hold, (LONG)(GetTickCount() + 3000));

    /* Burst the background snapshot while the switch is on; back to the idle
     * cadence otherwise (an idle desktop widget must cost nothing). */
    cap_set_live(want);

    /* A 4-16 ms backdrop cadence is impossible on the default 15.6 ms
     * scheduler tick: WaitForSingleObject(4) rounds up to a whole tick, which
     * is exactly why the live backdrop topped out at ~30 fps even though each
     * build only costs ~12 ms.  Hold the 1 ms tick whenever the worker is
     * armed, not just for the old exclusion fast path. */
    if (base && !g_live_hires)
    {
        timeBeginPeriod(1);
        g_live_hires = TRUE;
    }
    else if (!base && g_live_hires)
    {
        timeEndPeriod(1);
        g_live_hires = FALSE;
    }

    InterlockedExchange(&g_force, 1);
    if (g_wake)
        SetEvent(g_wake);
}

void glass_set_capture_visible(BOOL on)
{
    g_capture_want = on ? TRUE : FALSE;
    live_fast_sync();
}

void glass_set_widget_rect(int x, int y, int w, int h)
{
    g_widget_rect.left = x;
    g_widget_rect.top = y;
    g_widget_rect.right = x + w;
    g_widget_rect.bottom = y + h;
    g_widget_rect_valid = w > 0 && h > 0;
}

/* The context menu is capture-visible too, so it also lands in its own and
 * the card's backdrop grabs.  Remember its rect for the same punch-out. */
void glass_set_menu_rect(int x, int y, int w, int h)
{
    g_menu_rect.left = x;
    g_menu_rect.top = y;
    g_menu_rect.right = x + w;
    g_menu_rect.bottom = y + h;
    g_menu_rect_valid = w > 0 && h > 0;
}

static int fold_axis(int v, int lo, int hi)
{
    /* Triangle fold of v into [lo, hi]: reflection is C0-continuous at the
     * seams, so the filled patch never shows a boundary line against the
     * real pixels around it (the old top/bottom lerp did - it read as a
     * rectangular ghost with square corners through the glass). */
    int len = hi - lo + 1;
    int period, d;
    if (len <= 1)
        return lo;
    period = 2 * (len - 1);
    d = (v - lo) % period;
    if (d < 0)
        d += period;
    if (d >= len)
        d = period - d;
    return lo + d;
}

/* A mirrored fill is seamless at the seam, but it repeats whatever sat just
 * outside the window.  At blur_radius = 0 those repeats are readable text and
 * icons - the menu panel is large enough to show them clearly.  Low-pass only
 * the filled rectangle: repeats become a smooth colour field, the untouched
 * desktop around it stays sharp.  Two O(w*h) sliding-window passes. */
static void smooth_rect(BYTE *px, int cw, int ch,
                        int x0, int y0, int x1, int y1)
{
    /* 20 px is enough to turn a window edge into a gentle gradient and to
     * dissolve text, even when the user's blur slider sits at zero. */
    const int R = 20;
    const int win = 2 * R + 1;
    int bw = x1 - x0, bh = y1 - y0;
    BYTE *a, *b;
    int x, y, c;

    if (bw <= 0 || bh <= 0)
        return;
    a = (BYTE *)malloc((size_t)bw * bh * 4);
    b = (BYTE *)malloc((size_t)bw * bh * 4);
    if (!a || !b)
    {
        free(a);
        free(b);
        return;
    }

    for (y = 0; y < bh; y++)
        memcpy(a + (size_t)y * bw * 4,
               px + ((size_t)(y0 + y) * cw + x0) * 4,
               (size_t)bw * 4);

    for (y = 0; y < bh; y++)
    {
        int sum[4] = { 0, 0, 0, 0 };
        const BYTE *row = a + (size_t)y * bw * 4;
        BYTE *dst = b + (size_t)y * bw * 4;
        int k, xx;

        for (k = -R; k <= R; k++)
        {
            xx = k < 0 ? 0 : (k >= bw ? bw - 1 : k);
            for (c = 0; c < 4; c++) sum[c] += row[xx * 4 + c];
        }
        for (x = 0; x < bw; x++)
        {
            for (c = 0; c < 4; c++) dst[x * 4 + c] = (BYTE)(sum[c] / win);
            xx = x - R; if (xx < 0) xx = 0;
            for (c = 0; c < 4; c++)
                sum[c] -= row[xx * 4 + c];
            xx = x + R + 1; if (xx >= bw) xx = bw - 1;
            for (c = 0; c < 4; c++)
                sum[c] += row[xx * 4 + c];
        }
    }

    for (x = 0; x < bw; x++)
    {
        int sum[4] = { 0, 0, 0, 0 };
        int k, yy;

        for (k = -R; k <= R; k++)
        {
            yy = k < 0 ? 0 : (k >= bh ? bh - 1 : k);
            for (c = 0; c < 4; c++)
                sum[c] += b[((size_t)yy * bw + x) * 4 + c];
        }
        for (y = 0; y < bh; y++)
        {
            for (c = 0; c < 4; c++)
                a[((size_t)y * bw + x) * 4 + c] = (BYTE)(sum[c] / win);
            yy = y - R; if (yy < 0) yy = 0;
            for (c = 0; c < 4; c++)
                sum[c] -= b[((size_t)yy * bw + x) * 4 + c];
            yy = y + R + 1; if (yy >= bh) yy = bh - 1;
            for (c = 0; c < 4; c++)
                sum[c] += b[((size_t)yy * bw + x) * 4 + c];
        }
    }

    for (y = 0; y < bh; y++)
        memcpy(px + ((size_t)(y0 + y) * cw + x0) * 4,
               a + (size_t)y * bw * 4, (size_t)bw * 4);
    free(a);
    free(b);
}

static void punch_rect(BYTE *px, int cw, int ch, int ox, int oy,
                       const RECT *target, int smooth)
{
    RECT wr, cap;
    LONG x0, x1, y0, y1, x, y;

    if (!px || !target)
        return;

    cap.left = ox;      cap.top = oy;
    cap.right = ox + cw; cap.bottom = oy + ch;
    if (!IntersectRect(&wr, target, &cap))
        return;

    x0 = wr.left - ox;  x1 = wr.right - ox;
    y0 = wr.top - oy;   y1 = wr.bottom - oy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    if (x1 <= x0 || y1 <= y0)
        return;

    for (y = y0; y < y1; y++)
    {
        /* mirror from the nearer horizontal seam, then fold into the frame */
        int sy;
        if (y - y0 <= y1 - 1 - y)
            sy = fold_axis(y0 - 1 - (y - y0), 0, ch - 1);
        else
            sy = fold_axis(y1 + (y1 - 1 - y), 0, ch - 1);
        BYTE *d = px + (size_t)y * cw * 4;
        for (x = x0; x < x1; x++)
        {
            int sx;
            if (x - x0 <= x1 - 1 - x)
                sx = fold_axis(x0 - 1 - (x - x0), 0, cw - 1);
            else
                sx = fold_axis(x1 + (x1 - 1 - x), 0, cw - 1);
            const BYTE *sp = px + (size_t)sy * cw * 4 + (size_t)sx * 4;
            BYTE *dp = d + (size_t)x * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }

    if (smooth)
        smooth_rect(px, cw, ch, (int)x0, (int)y0, (int)x1, (int)y1);
}

/* Reconstruct the pixels our own layered windows hid with a low-frequency
 * copy of the SAME frame.  A mirror fill switches source at the half-way line
 * (vertical split) and again on both axes (diagonal seam); at blur = 0 that
 * reads as two different desktops glued together, with repeated, still
 * legible text.  A coarse average of the untouched frame has no source switch
 * at all: it is exactly what frosted glass shows - a diffused version of
 * whatever is behind the card - and overlapping rects are filled from one
 * common field, so no order-dependent patch is possible. */
static BOOL punch_coarse(BYTE *px, int cw, int ch, int ox, int oy)
{
    RECT cap, list[2], uni, hit;
    int n = 0, R = 20, dx0, dy0, dx1, dy1, bw, bh, i, y;
    BYTE *tmp;

    cap.left = ox;  cap.top = oy;
    cap.right = ox + cw; cap.bottom = oy + ch;
    memset(&uni, 0, sizeof(uni));

    if (g_widget_rect_valid && IntersectRect(&hit, &g_widget_rect, &cap))
    {
        list[n++] = hit;
        uni = hit;
    }
    if (g_menu_rect_valid && IntersectRect(&hit, &g_menu_rect, &cap))
    {
        list[n++] = hit;
        if (n == 1) uni = hit;
        else UnionRect(&uni, &uni, &hit);
    }
    if (n == 0)
        return TRUE;

    dx0 = uni.left - R;   if (dx0 < cap.left)   dx0 = cap.left;
    dy0 = uni.top - R;    if (dy0 < cap.top)    dy0 = cap.top;
    dx1 = uni.right + R;  if (dx1 > cap.right)  dx1 = cap.right;
    dy1 = uni.bottom + R; if (dy1 > cap.bottom) dy1 = cap.bottom;
    bw = dx1 - dx0;
    bh = dy1 - dy0;
    if (bw <= 0 || bh <= 0)
        return FALSE;

    tmp = (BYTE *)malloc((size_t)bw * bh * 4);
    if (!tmp)
        return FALSE;

    for (y = 0; y < bh; y++)
        memcpy(tmp + (size_t)y * bw * 4,
               px + ((size_t)(dy0 - oy + y) * cw + (dx0 - ox)) * 4,
               (size_t)bw * 4);

    /* Two separable box passes: ~58 px effective diffusion.  Text, icons and
     * the hard edge of a window behind the glass become one smooth field. */
    smooth_rect(tmp, bw, bh, 0, 0, bw, bh);
    smooth_rect(tmp, bw, bh, 0, 0, bw, bh);

    for (i = 0; i < n; i++)
    {
        int x0 = (int)(list[i].left - ox), x1 = (int)(list[i].right - ox);
        int y0 = (int)(list[i].top - oy),  y1 = (int)(list[i].bottom - oy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > cw) x1 = cw;
        if (y1 > ch) y1 = ch;
        for (y = y0; y < y1; y++)
            memcpy(px + ((size_t)y * cw + x0) * 4,
                   tmp + ((size_t)(y - (dy0 - oy)) * bw +
                          (x0 - (dx0 - ox))) * 4,
                   (size_t)(x1 - x0) * 4);
    }
    free(tmp);
    return TRUE;
}

/* Fallback reconstruction, used only when SetWindowDisplayAffinity could not
 * hide our windows from the grab.  With a clean capture this is never called,
 * so the real desktop (including its fine detail) survives untouched. */
static void punch_capture(BYTE *px, int cw, int ch, int ox, int oy, int smooth)
{
    if (!px || !g_capture_visible)
        return;
    if (punch_coarse(px, cw, ch, ox, oy))
        return;
    if (g_widget_rect_valid)
        punch_rect(px, cw, ch, ox, oy, &g_widget_rect, smooth);
    if (g_menu_rect_valid)
        punch_rect(px, cw, ch, ox, oy, &g_menu_rect, smooth);
}

/* Builds the live tile around (x,y,w,h) with margin, then blurs it. */
static BOOL tile_build(int x, int y, int w, int h, float radius)
{
    RectI mon = monitor_rect_at(x + w / 2, y + h / 2);

    /* Before the first UpdateLayeredWindow there is no self surface to
     * un-composite: the card is on screen but its pixels cannot be told
     * apart from the desktop, and any reconstruction here is a guess that
     * the "same" freeze would keep forever.  One skipped build costs
     * milliseconds; a poisoned tile costs the whole session. */
    if (g_capture_visible && !g_seed_mode &&
        IsWindowVisible(g_notify) && !g_self[0].valid)
        return FALSE;
    /* The affinity flip reaches the capture path on the next composition.
     * Wait for it here (worker thread), not in live_fast_sync() where the UI
     * toggle would stall for 35 ms and feel like the switch did nothing. */
    if (g_live_fast_on)
    {
        LONG since = InterlockedCompareExchange(&g_fast_since, 0, 0);
        if (since && (DWORD)GetTickCount() - (DWORD)since < 35)
            return FALSE;
    }
    int m = live_margin();

    /* A parked card does not travel: the 80 px slack only exists for small
     * moves between rebuilds.  At 120 fps the grab size IS the frame budget
     * (a 768x561 BitBlt costs ~5 ms), so the live path grabs a tight box. */
    if (m > LIVE_MARGIN && InterlockedCompareExchange(&g_live_fast, 0, 0) &&
        !InterlockedCompareExchange(&g_hold, 0, 0))
        m = LIVE_MARGIN;
    int cx = x - m, cy = y - m, cw = w + m * 2, ch = h + m * 2;
    /* A zero-blur backdrop is read as clear glass: half-res would soften it
     * and make the texture swim as the card moves.  Blurred tiles lose
     * nothing at half resolution, so only the sharp case pays for full size. */
    int scale = radius < 0.75f ? 1 : PLANE_SCALE;
    BYTE *full = NULL;
    Plane p;

    if (cx < mon.x) { cw -= mon.x - cx; cx = mon.x; }
    if (cy < mon.y) { ch -= mon.y - cy; cy = mon.y; }
    if (cx + cw > mon.x + mon.w) cw = mon.x + mon.w - cx;
    if (cy + ch > mon.y + mon.h) ch = mon.y + mon.h - cy;
    if (cw <= 4 || ch <= 4)
        return FALSE;

    /* The plate is the only source of truth for the pixels under a card that
     * sits inside the capture, so it has to be re-learned from the truth on a
     * slow clock - not on a change.
     *
     * Two separate things defeated every earlier attempt at this:
     *
     *  - The request used to be gated on g_live_idle.  With that switch off
     *    (the case where the card is ALWAYS in the grab, because live_fast_sync
     *    can never exclude it) the flag was drained by the InterlockedExchange
     *    and then thrown away on the next term.
     *
     *  - Even with that door open, nobody knocked.  self_erase() skips every
     *    pixel the card covers (self_hole_at), so its change detector only ever
     *    sees the band AROUND the card.  A tooltip, a popup or a notification
     *    that lived and died entirely under the glass produced no request at
     *    all, and the plate went on showing it after it was gone.  That is the
     *    residue people report: a Windows file tooltip frozen behind the glass.
     *
     * So: refresh on the clock while the card is capture-visible and idle.  An
     * explicit request still wins, so a change seen in the band is repaired on
     * the spot rather than at the next tick.
     *
     * Cost: one compositor turn (~35 ms of Sleep, no CPU) plus a BitBlt of the
     * card box and a per-pixel write into the plate - roughly 3 ms of real work
     * per second, about 0.3 % of one core, and only while the card is still.
     * !g_hold keeps it off a drag, where the 35 ms wait would be felt. */
    {
        ULONGLONG t0 = GetTickCount64();
        if (!capture_rect(cx, cy, cw, ch, &full))
            return FALSE;
        InterlockedExchangeAdd(&g_cap_us, (LONG)(GetTickCount64() - t0) * 1000);
        InterlockedIncrement(&g_builds);
    }

    /* capture_rect() already excluded our own windows from this grab. */

    memset(&p, 0, sizeof(p));
    if (!plane_alloc(&p, cw / scale, ch / scale))
    {
        free(full);
        return FALSE;
    }
    p.scale = scale;

    if (scale == 1)
        memcpy(p.raw, full, (size_t)p.w * p.h * 4);
    else
        downsample2(full, cw, ch, p.raw, p.w, p.h);
    free(full);

    /* Nothing moved behind the card: keep the tile that is already published
     * instead of pushing an identical frame through the compositor.  This is
     * what keeps an idle desktop widget at ~1% of a core. */
    {
        BOOL same = FALSE;
        EnterCriticalSection(&g_live_lock);
        same = g_ltp.valid && g_ltp.raw && g_ltp.w == p.w && g_ltp.h == p.h &&
               g_ltp.scale == p.scale &&
               g_ltp.x == cx && g_ltp.y == cy &&
               fabsf(g_ltp.radius - radius) < 0.6f &&
               memcmp(g_ltp.raw, p.raw, (size_t)p.w * p.h * 4) == 0;
        LeaveCriticalSection(&g_live_lock);
        if (same)
        {
            plane_free(&p);
            InterlockedExchange(&g_tile_same, 1);
            InterlockedExchange(&g_diff_run, 0);
            return TRUE;
        }
    }

    memcpy(p.px, p.raw, (size_t)p.w * p.h * 4);
    if (radius > 0.01f)
        blur_bgra(p.px, p.w, p.h, radius / (float)scale);

    p.x = cx;
    p.y = cy;
    p.radius = radius;
    p.valid = TRUE;

    publish_plane(&p);
    plane_free(&p);       /* the buffers the swap handed back */
    InterlockedExchange(&g_tile_same, 0);
    /* Three differing frames in a row is animation; one is a window being
     * dragged past, a cursor, a clock tick.  Only real motion is worth taking
     * the card out of screenshots for. */
    if (InterlockedIncrement(&g_diff_run) >= 5)
        InterlockedExchange(&g_seen_motion, (LONG)GetTickCount());
    return TRUE;
}

/* TRUE when every sample point inside the card belongs to another process'
 * window: a maximized app, a full-screen game, or any window parked over the
 * widget.  Grabbing cannot show anything then, so the worker idles. */
static BOOL card_hidden(void)
{
    static const int ins[5][2] = { {50,50}, {10,50}, {90,50}, {50,10}, {50,90} };
    DWORD self_pid = GetCurrentProcessId();
    RECT r;
    int i;

    if (!g_notify || !GetWindowRect(g_notify, &r) ||
        r.right <= r.left || r.bottom <= r.top)
        return FALSE;
    for (i = 0; i < 5; i++)
    {
        POINT p;
        HWND h;
        DWORD pid = 0;

        p.x = r.left + (r.right - r.left) * ins[i][0] / 100;
        p.y = r.top + (r.bottom - r.top) * ins[i][1] / 100;
        h = WindowFromPoint(p);
        if (!h)
            return FALSE;
        GetWindowThreadProcessId(h, &pid);
        if (pid == self_pid)
            return FALSE;
    }
    return TRUE;
}


/* Microsecond clock.  GetTickCount64() only steps on a timer tick (15.6 ms at
 * the default resolution), which quantised the pacing below a frame: two build
 * decisions in the same tick compared as "0 ms elapsed" and one of them was
 * dropped, halving the live rate. */
static ULONGLONG now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;

    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart * 1000000ULL / (ULONGLONG)f.QuadPart);
}

static DWORD WINAPI live_worker(void *arg)
{
    ULONGLONG last = 0;
    DWORD wait_ms = LIVE_IDLE_MS;
    int same_run = 0;
    (void)arg;

    for (;;)
    {
        int tx, ty, tw, th;
        float tr, cur_radius = 0.0f;
        BOOL forced, stale, have, hold;
        DWORD poll;
        ULONGLONG now;

        hold = InterlockedCompareExchange(&g_hold, 0, 0) != 0;
        poll = hold ? LIVE_SLOW_MS : wait_ms;
        WaitForSingleObject(g_wake, poll);

        if (InterlockedCompareExchange(&g_quit, 0, 0))
            break;
        if (InterlockedCompareExchange(&g_suspend, 0, 0))
        {
            last = 0;               /* rebuild as soon as we are back */
            wait_ms = LIVE_STILL_POLL_MS;
            continue;
        }
        if (InterlockedCompareExchange(&g_menu_open, 0, 0))
            continue;
        if (!InterlockedCompareExchange(&g_target_set, 0, 0))
        {
            /* Paused (the card is capture-visible and reads a frozen plane):
             * sleep on the slow tick.  Keeping the previous wait_ms here left a
             * 6 ms wake-up loop running for a worker that has nothing to do. */
            wait_ms = LIVE_STILL_POLL_MS;
            cap_set_demand(FALSE);
            continue;
        }

        EnterCriticalSection(&g_live_lock);
        tx = g_tx; ty = g_ty; tw = g_tw; th = g_th; tr = g_tr;
        have = g_ltp.valid;
        cur_radius = g_ltp.radius;
        LeaveCriticalSection(&g_live_lock);

        if (tw <= 0 || th <= 0)
            continue;

        /* Follow the motion state: the card is only kept out of capture while
         * something behind it actually moves (see live_motion_fast).  Never
         * mid-drag: the drag path decides per frame whether it must
         * un-composite the card, and an affinity flip takes a composition to
         * land - a frame grabbed inside that window would bake the card in. */
        /* Compare against the mode this switch can actually enter.  While the
         * card must stay capture-visible, live_fast_sync() always returns
         * "not fast"; feeding it raw motion here made the two states differ on
         * every loop, so it re-forced the worker each time and bypassed all
         * pacing (observed: ~90 fps of full grabs for a 1 Hz plate). */
        if (!InterlockedCompareExchange(&g_hold, 0, 0))
        {
            BOOL desired_fast = live_motion_fast() && !g_capture_want;
            if (desired_fast != (g_live_fast_on != 0))
                live_fast_sync();
        }
        /* WindowFromPoint walks the window tree five times; at 120 fps that is
         * a fifth of the frame budget for a condition that changes on human
         * time.  Sample it every 100 ms. */
        {
            static ULONGLONG hidden_at;
            static BOOL hidden;

            if (!hidden_at || GetTickCount64() - hidden_at > 100)
            {
                hidden_at = GetTickCount64();
                hidden = card_hidden();
            }
            if (hidden)
            {
                last = 0;           /* nothing visible: no grabs at all */
                wait_ms = LIVE_COVERED_MS;
                continue;
            }
        }

        now = now_us();
        forced = InterlockedExchange(&g_force, 0) != 0;
        hold = InterlockedCompareExchange(&g_hold, 0, 0) != 0;
        /* A drag paints frame-exact grabs on the UI thread and owns the
         * visuals completely.  Any tile grab running meanwhile steals the
         * same compositor readback and shows up as a ~40 ms hitch every time
         * the card leaves the tile's coverage - so the worker sleeps through
         * the whole drag and rebuilds once it ends. */
        if (hold)
        {
            wait_ms = LIVE_STILL_POLL_MS;
            continue;
        }
        poll = wait_ms;

        if (!forced && last && now - last < (ULONGLONG)LIVE_MIN_MS * 1000)
            continue;

        /* Rebuild when the tile cannot serve the card, when the radius
         * changed, when the caller forced it, or - while the card is still -
         * on the poll interval, so a video behind it still shows through.
         * After a few identical captures the interval doubles, up to 400 ms:
         * a static desktop should not cost more than a percent of a core. */
        /* With the live switch off the worker still has to LOOK at the screen
         * now and then: a window that moved or closed under the card leaves
         * the plate showing pixels that no longer exist ("leftover" after
         * switching the live background off), and the only detector is the
         * visible band around the card.  This used to floor the interval at
         * 400 ms; at ~1 ms a build, the still cadence can be the same 100 ms
         * the live path uses, which makes that self-heal four times sooner. */
        if (!InterlockedCompareExchange(&g_live_idle, 0, 0) && poll < LIVE_STILL_POLL_MS)
            poll = LIVE_STILL_POLL_MS;

        stale = !have || !tile_covers(tx, ty, tw, th) ||
                fabsf(cur_radius - tr) > 0.01f ||
                now - last >= (ULONGLONG)poll * 1000;

        if (!forced && !stale)
            continue;

        if (tile_build(tx, ty, tw, th, tr))
        {
            last = now;
            if (InterlockedCompareExchange(&g_tile_same, 0, 0))
            {
                /* One identical frame is normal when we run slightly faster
                 * than the compositor; only a RUN of them means the screen is
                 * actually still, and only then is it worth backing off. */
                same_run++;
                /* Back off, but never past the point where the glass reads as
                 * laggy.  The old ceilings (400 ms, then 1000 ms after a long
                 * still spell) traded a second of latency for under a percent
                 * of a core - the wrong way round, given a build costs ~1 ms. */
                if (same_run >= 10 && wait_ms < LIVE_STILL_POLL_MS)
                {
                    DWORD next = wait_ms * 2;
                    wait_ms = next > LIVE_STILL_POLL_MS ? LIVE_STILL_POLL_MS : next;
                }
            }
            else
            {
                /* Something moved.  A still card with the live switch on keeps
                 * the live cadence (the glass body really is live now, so the
                 * interval is what the eye sees); without the switch it was
                 * rebuilt from the plate at the repair rate anyway. */
                same_run = 0;
                /* Only the fast path needs 120+ fps.  In the plate path the
                 * pixels under the card come from a once-a-second repair, so
                 * a 12 ms rebuild rate was 64 fps of full-screen grabs whose
                 * under-card result could not change - about half a core for
                 * nothing.  Ten looks a second keep the visible rim responsive
                 * and still detect motion quickly. */
                wait_ms = g_live_fast_on ? LIVE_STILL_MS :
                          (g_capture_want ? PLATE_PROBE_MS : LIVE_PROBE_MS);
            }

            /* Nothing moved for a while: let the snapshot stop bursting, or a
             * 120 fps-capable capture thread would burn a percent of a core on
             * a static wallpaper. */
            cap_set_demand(wait_ms < LIVE_STILL_POLL_MS);
        }
    }

    return 0;
}

/* Stops (pause = TRUE) or re-arms the live capture worker.  The card only
 * samples the screen while it is always-on-top; as a plain desktop widget it
 * reads the desktop instead and this worker must go quiet. */
void glass_live_pause(BOOL pause)
{
    live_init();
    InterlockedExchange(&g_target_set, pause ? 0 : 1);
    /* A paused worker has no grabs to feed: drop the capture exclusion too. */
    live_fast_sync();
}

/* Session lock/unlock: while the workstation is locked no capture can succeed
 * and nothing of the card is visible, so the worker sleeps until unlocked. */
void glass_live_suspend(BOOL suspend)
{
    live_init();
    InterlockedExchange(&g_suspend, suspend ? 1 : 0);
    InterlockedExchange(&g_force, 1);
    if (g_wake)
        SetEvent(g_wake);
}

void glass_live_set_target(HWND hwnd)
{
    live_init();
    g_notify = hwnd;

    if (!g_wake)
        g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_thread && g_wake)
        g_thread = CreateThread(NULL, 0, live_worker, NULL, 0, NULL);
    /* The exclusion can only be applied once the window exists. */
    live_fast_sync();
}

void glass_live_follow(int x, int y, int w, int h, float blur_radius, BOOL hold)
{
    BOOL changed, radius_ok, near_edge = FALSE;
    BOOL drag_start;
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
    /* Keep a margin of runway while dragging: ask for a fresh tile well
     * before the card reaches the edge, so no frame has to stretch the last
     * row/column of the old one. */
    if (hold && g_ltp.valid)
    {
        int lead = PLANE_MARGIN_DRAG / 2;
        near_edge = x - g_lrect.x < lead || y - g_lrect.y < lead ||
                    g_lrect.x + g_lrect.w - (x + w) < lead ||
                    g_lrect.y + g_lrect.h - (y + h) < lead;
    }
    LeaveCriticalSection(&g_live_lock);

    InterlockedExchange(&g_target_set, 1);
    {
        LONG was_hold = InterlockedCompareExchange(&g_hold, 0, 0);
        InterlockedExchange(&g_hold, hold ? 1 : 0);
        /* The idle tile carries the small margin.  Rebuild it once, on
         * mouse-down, so the bigger drag margin and the capture that fills it
         * are ready before the card can outrun the old one. */
        drag_start = hold && !was_hold;
    }

    /* The tile is bigger than the card, so most still moves are served
     * straight out of it without a capture.  A drag is different: the card
     * travels over a live desktop, and a tile that is merely covered is not
     * a tile that is current - waiting for the runway edge left the glass
     * showing the screen from before the drag started (measured 150 ms, up
     * to 1 s, for the first drag after the card sat still).  While the drag
     * is held, wake the worker on every move without forcing it: its own
     * pacing (LIVE_DRAG_MS) keeps the tile within a frame, and a still
     * screen still backs off. */
    {
        BOOL force = drag_start ||
                     (changed && (!radius_ok || !tile_covers(x, y, w, h) || near_edge));

        if (force || (changed && hold))
        {
            if (force)
                InterlockedExchange(&g_force, 1);
            if (g_wake)
                SetEvent(g_wake);
        }
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

BOOL glass_live_rim(int x, int y, int w, int h, RgBitmap *out)
{
    BOOL ok;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;

    live_init();
    EnterCriticalSection(&g_live_lock);
    ok = plane_crop_raw(&g_ltp, x, y, w, h, out);
    LeaveCriticalSection(&g_live_lock);
    return ok;
}

/* Light un-composite for the frame-exact grab.  The heavy self_erase() plate
 * learner stays with the worker; here the card is punched at the position of
 * the last upload, inflated to cover the one or two composed frames a
 * flush-less grab may lag by.  The blur that follows hides the mirror fill. */
static void self_punch_light(BYTE *px, int cw, int ch, int ox, int oy)
{
    RECT cap, hit;
    RECT holes[2];
    int n = 0, i;
    /* Just enough to cover one frame of composition lag / pointer travel;
     * more would eat the refraction band's real desktop for no benefit. */
    const int inflate = 12;

    cap.left = ox;
    cap.top = oy;
    cap.right = ox + cw;
    cap.bottom = oy + ch;

    InitOnceExecuteOnce(&g_cap_once, cap_once, NULL, NULL);
    EnterCriticalSection(&g_cap_lock);
    for (i = 0; i < 2; i++)
    {
        const SelfLayer *L = &g_self[i];
        RECT r;
        if (!L->valid || !L->px || L->w <= 0 || L->h <= 0)
            continue;
        r.left = L->x - inflate;
        r.top = L->y - inflate;
        r.right = L->x + L->w + inflate;
        r.bottom = L->y + L->h + inflate;
        holes[n++] = r;
    }
    LeaveCriticalSection(&g_cap_lock);

    for (i = 0; i < n; i++)
    {
        if (IntersectRect(&hit, &holes[i], &cap))
            punch_rect(px, cw, ch, ox, oy, &hit, 0);
    }

    /* No surface history yet (first frames): fall back to the widget rect. */
    if (n == 0 && g_widget_rect_valid &&
        IntersectRect(&hit, &g_widget_rect, &cap))
        punch_rect(px, cw, ch, ox, oy, &hit, 0);
}

/* RG_PROF=1 stage timings for the drag fast path (glass.log). */
static ULONGLONG qpc_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)c.QuadPart * 1000000ULL / freq.QuadPart;
}

static void drag_stage_log(int stage, ULONGLONG us)
{
    static ULONGLONG acc[4], helper_us;
    static unsigned n, n_helper;
    static int last_w, last_h;
    wchar_t base[MAX_PATH], path[MAX_PATH], v[8];
    FILE *f;

    if (stage >= 0 && stage < 4)
        acc[stage] += us;
    if (stage == 10) { helper_us += us; n_helper++; }
    if (stage == 14) last_w = (int)us;
    if (stage == 15) last_h = (int)us;
    if (stage >= 0)
        return;
    if (++n < 90)
        return;
    if (!GetEnvironmentVariableW(L"RG_PROF", v, 8) || v[0] != L'1')
    {
        memset(acc, 0, sizeof(acc));
        helper_us = 0;
        n = n_helper = 0;
        return;
    }
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    swprintf_s(path, MAX_PATH, L"%ls\\%ls\\glass.log", base, RG_APP_NAME);
    f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f)
        return;
    fwprintf(f, L"drag %dx%d grab=%.0f helper=%.0f/%u blur=%.0f crop=%.0f us\n",
             last_w, last_h,
             (double)(acc[0] / n),
             n_helper ? (double)(helper_us / n_helper) : 0.0, n_helper,
             (double)(acc[2] / n), (double)(acc[3] / n));
    fclose(f);
    memset(acc, 0, sizeof(acc));
    helper_us = 0;
    n = n_helper = 0;
}

/* Frame-exact synchronous backdrop for the drag fast path.
 *
 * The worker tile is always at least one round-trip old; at drag speed that
 * lag is the "camera jelly": the glass shows the screen as it was tens to
 * hundreds of milliseconds ago, and a crop miss re-pins the previous frame's
 * bitmap to the new position so the desktop appears to swim with the card.
 * This path grabs exactly the frame's rect on the calling thread - no helper
 * IPC, no DwmFlush, no worker round-trip - so the painted backdrop is the
 * screen as it is in this very frame.  Cost: one small BitBlt plus a
 * half-resolution blur.
 *
 * out     receives the blurred crop (glass body).
 * out_rim optionally receives the unblurred crop (refraction band).
 */
BOOL glass_drag_backdrop(int x, int y, int w, int h, float blur_radius,
                         RgBitmap *out, RgBitmap *out_rim)
{
    /* Frame-exact synchronous backdrop for the drag fast path.
     *
     * The worker tile is always at least one round-trip old; at drag speed
     * that lag is the "camera jelly", and the old bitmap was pinned to the
     * window so the desktop appeared to swim with the card.  This path grabs
     * the exact frame on the calling thread.
     *
     * The grab is BitBlt plus the plate-learning self_erase(), with a
     * learning margin around the card so the plate keeps warming as the card
     * travels.  Pixels under the card come from the plate (the remembered
     * desktop - the seed learned them before the card covered them); only
     * pixels the plate has truly never seen get diffused.  The light mirror
     * punch stays as the last resort and must not be the norm: at blur 0 its
     * fill is visible as smeared "mirror" text on the glass.
     */
    BITMAPINFO bi;
    HDC screen = NULL;
    HGDIOBJ old;
    BYTE *card = NULL;
    Plane p;
    float r = blur_radius * g_blur_scale;
    int scale = r < 0.75f ? 1 : PLANE_SCALE;
    int pw, ph;
    /* UI-thread-only scratch: a drag pays this path every frame, so the
     * buffers are grown once and kept instead of malloc'd per grab. */
    static BYTE *s_full, *s_card, *s_raw, *s_blur;
    static int s_full_cap, s_card_cap, s_plane_cap;
    static HDC s_mem;
    static HBITMAP s_dib;
    static void *s_bits;
    static int s_dib_w, s_dib_h;
    /* Screen-space cache of the last un-blended grab, served to every frame
     * until the card leaves it (see the block below). */
    static BYTE *s_cache;
    static int s_cache_x, s_cache_y, s_cache_w, s_cache_h;
    static ULONGLONG s_cache_at;
    ULONGLONG tq = qpc_us();
    BOOL got = FALSE;
    BOOL widget_in_frame = g_capture_visible && !g_seed_mode;

    if (w <= 0 || h <= 0 || !out)
        return FALSE;

    memset(&p, 0, sizeof(p));
    pw = (w + scale - 1) / scale;
    ph = (h + scale - 1) / scale;

    if ((size_t)w * h * 4 > (size_t)s_card_cap)
    {
        BYTE *nb = (BYTE *)realloc(s_card, (size_t)w * h * 4);
        if (!nb)
            return FALSE;
        s_card = nb;
        s_card_cap = w * h * 4;
    }
    if ((size_t)pw * ph * 4 > (size_t)s_plane_cap)
    {
        BYTE *nr = (BYTE *)realloc(s_raw, (size_t)pw * ph * 4);
        BYTE *nb = (BYTE *)realloc(s_blur, (size_t)pw * ph * 4);
        if (!nr || !nb)
        {
            if (nr) s_raw = nr;
            if (nb) s_blur = nb;
            return FALSE;
        }
        s_raw = nr;
        s_blur = nb;
        s_plane_cap = pw * ph * 4;
    }
    card = s_card;
    p.raw = s_raw;
    p.px = s_blur;
    p.w = pw;
    p.h = ph;
    p.scale = scale;

    /* Screen-space cache: one grab serves every frame until the card travels
     * out of it.
     *
     * Re-grabbing per frame is what produced the motion smear.  A readback
     * takes several milliseconds, DWM composes two or three card positions
     * inside that window, and the un-blend can only cancel the one surface it
     * knows - so the extra copies stay in the backdrop and show up inside the
     * glass as ghost text and ghost rims.  Grabbing once per ~220 px of travel
     * removes both the smear and the per-frame readback cost. */
    {
        RectI mon = monitor_rect_at(x + w / 2, y + h / 2);
        int runway = widget_in_frame ? 96 : 0;
        BOOL covered = s_cache && s_cache_w > 0 &&
                       GetTickCount64() - s_cache_at < 1500 &&
                       x - runway >= s_cache_x && y - runway >= s_cache_y &&
                       x + w + runway <= s_cache_x + s_cache_w &&
                       y + h + runway <= s_cache_y + s_cache_h;

        if (!covered)
        {
            int m = widget_in_frame ? 220 : 0;
            int gx = x - m, gy = y - m, gw = w + m * 2, gh = h + m * 2;

            if (gx < mon.x) { gw -= mon.x - gx; gx = mon.x; }
            if (gy < mon.y) { gh -= mon.y - gy; gy = mon.y; }
            if (gx + gw > mon.x + mon.w) gw = mon.x + mon.w - gx;
            if (gy + gh > mon.y + mon.h) gh = mon.y + mon.h - gy;
            if (gw < w || gh < h)
            {
                gx = x; gy = y; gw = w; gh = h;
            }

            if ((size_t)gw * gh * 4 > (size_t)s_full_cap)
            {
                BYTE *nb = (BYTE *)realloc(s_full, (size_t)gw * gh * 4);
                if (!nb)
                    return FALSE;
                s_full = nb;
                s_full_cap = gw * gh * 4;
            }

            if (!s_mem || s_dib_w != gw || s_dib_h != gh)
            {
                if (s_dib)
                {
                    DeleteObject(s_dib);
                    s_dib = NULL;
                }
                if (!s_mem)
                {
                    screen = GetDC(NULL);
                    if (!screen)
                        return FALSE;
                    s_mem = CreateCompatibleDC(screen);
                    ReleaseDC(NULL, screen);
                    if (!s_mem)
                        return FALSE;
                }
                memset(&bi, 0, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = gw;
                bi.bmiHeader.biHeight = -gh;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                s_dib = CreateDIBSection(s_mem, &bi, DIB_RGB_COLORS, &s_bits,
                                         NULL, 0);
                if (!s_dib || !s_bits)
                {
                    s_dib = NULL;
                    return FALSE;
                }
                s_dib_w = gw;
                s_dib_h = gh;
            }

            if (!widget_in_frame && cap_grab(gx, gy, gw, gh, s_full))
            {
                got = TRUE;
            }
            else
            {
                /* One composition wait so the frame contains the surface this
                 * un-blend is about to subtract, at the position it is stored
                 * at.  Without it the subtraction lands one move away and the
                 * card's own pixels survive as a ghost. */
                if (widget_in_frame)
                    glass_present();
                old = SelectObject(s_mem, s_dib);
                screen = GetDC(NULL);
                if (screen)
                {
                    BOOL blt = BitBlt(s_mem, 0, 0, gw, gh, screen, gx, gy, SRCCOPY);
                    if (!blt)
                        blt = BitBlt(s_mem, 0, 0, gw, gh, screen, gx, gy,
                                     SRCCOPY | CAPTUREBLT);
                    if (blt)
                    {
                        memcpy(s_full, s_bits, (size_t)gw * gh * 4);
                        got = TRUE;
                    }
                    ReleaseDC(NULL, screen);
                }
                SelectObject(s_mem, old);
            }
            if (!got)
                return FALSE;

            if (widget_in_frame &&
                !self_unblend(s_full, gw, gh, gx, gy))
                self_punch_light(s_full, gw, gh, gx, gy);

            s_cache = s_full;
            s_cache_x = gx;
            s_cache_y = gy;
            s_cache_w = gw;
            s_cache_h = gh;
            s_cache_at = GetTickCount64();
        }

        /* Serve this frame out of the cache. */
        {
            int row;
            const BYTE *srow = s_cache +
                ((size_t)(y - s_cache_y) * (size_t)s_cache_w +
                 (size_t)(x - s_cache_x)) * 4;

            for (row = 0; row < h; row++)
                memcpy(card + (size_t)row * w * 4,
                       srow + (size_t)row * s_cache_w * 4, (size_t)w * 4);
        }
    }

    drag_stage_log(0, qpc_us() - tq);
    drag_stage_log(14, (ULONGLONG)w);
    drag_stage_log(15, (ULONGLONG)h);

    /* ---- blur + crops ---------------------------------------------- */
    p.x = x;
    p.y = y;
    p.radius = r;
    p.valid = TRUE;

    /* GPU fast path: the whole downsample -> blur -> upsample chain on the
     * card, replacing ~2 ms of CPU blur + 2x bilinear crop.  The rim comes
     * from the full-res grab (sharper than the old half-res upsample and a
     * plain memcpy).  Any GPU failure falls through to the CPU path, which
     * stays as the verified reference. */
    if (r >= 0.75f)
    {
        ULONGLONG tb = qpc_us();
        BYTE *gb = (BYTE *)malloc((size_t)w * h * 4);

        if (gb)
        {
            if (gpu_blur2x(card, gb, w, h, r))
            {
                out->w = w;
                out->h = h;
                out->px = gb;
                if (out_rim)
                {
                    BYTE *rb = (BYTE *)malloc((size_t)w * h * 4);
                    if (rb)
                    {
                        memcpy(rb, card, (size_t)w * h * 4);
                        out_rim->w = w;
                        out_rim->h = h;
                        out_rim->px = rb;
                    }
                }
                drag_stage_log(2, qpc_us() - tb);
                drag_stage_log(3, 0);
                drag_stage_log(-1, 0);
                InterlockedExchange(&g_sync_tick, (LONG)GetTickCount64());
                return TRUE;
            }
            free(gb);
        }
    }

    {
    ULONGLONG tb = qpc_us();
    if (scale == 1)
        memcpy(p.raw, card, (size_t)pw * ph * 4);
    else
        downsample2(card, w, h, p.raw, pw, ph);

    memcpy(p.px, p.raw, (size_t)pw * ph * 4);
    if (r > 0.01f)
        blur_bgra(p.px, pw, ph, r / (float)scale);
    drag_stage_log(2, qpc_us() - tb);
    }

    {
    ULONGLONG tc = qpc_us();
    if (!plane_crop(&p, x, y, w, h, out))
        return FALSE;
    if (out_rim)
        plane_crop_raw(&p, x, y, w, h, out_rim);
    drag_stage_log(3, qpc_us() - tc);
    drag_stage_log(-1, 0);
    }

    InterlockedExchange(&g_sync_tick, (LONG)GetTickCount64());
    return TRUE;
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

/* Drops the frozen screen so the plane goes back to holding the wallpaper. */
void glass_unfreeze_plane(void)
{
    if (!InterlockedExchange(&g_wpp_screen, 0))
        return;
    g_wpp.valid = FALSE;
}

/* Half-resolution variant of glass_blurred_copy: downsample 2x, blur, then
 * bilinear upscale.  The blur is the dominant cost of opening the menu, and
 * doing it at half size is ~4x cheaper; the result is what the live backdrop
 * pipeline has been producing all along. */
BOOL glass_blur_half(const RgBitmap *src, float radius, RgBitmap *out)
{
    int hw = src->w / 2, hh = src->h / 2;
    BYTE *half, *up;
    size_t bytes;
    int x, y;

    if (!src || !src->px || !out || hw < 4 || hh < 4)
        return FALSE;

    half = (BYTE *)malloc((size_t)hw * hh * 4);
    if (!half)
        return FALSE;

    downsample2(src->px, src->w, src->h, half, hw, hh);
    blur_bgra(half, hw, hh, radius * g_blur_scale * 0.5f);

    bytes = (size_t)src->w * src->h * 4;
    up = (BYTE *)malloc(bytes);
    if (!up)
    {
        free(half);
        return FALSE;
    }

    for (y = 0; y < src->h; y++)
    {
        float fy = ((float)y + 0.5f) * 0.5f - 0.5f;
        int iy = (int)fy;
        int iy1;
        float ty;
        BYTE *drow = up + (size_t)y * src->w * 4;

        if (iy < 0) { iy = 0; ty = 0.0f; }
        if (iy > hh - 1) { iy = hh - 1; ty = 0.0f; }
        iy1 = iy + 1 < hh ? iy + 1 : iy;
        ty = fy - (float)iy;
        if (ty < 0.0f) ty = 0.0f; else if (ty > 1.0f) ty = 1.0f;

        for (x = 0; x < src->w; x++)
        {
            float fx = ((float)x + 0.5f) * 0.5f - 0.5f;
            int ix = (int)fx, ix1;
            float tx, w00, w01, w10, w11;
            const BYTE *a, *b, *cc, *d;
            BYTE *o = drow + (size_t)x * 4;
            int k;

            if (ix < 0) { ix = 0; tx = 0.0f; }
            if (ix > hw - 1) { ix = hw - 1; tx = 0.0f; }
            ix1 = ix + 1 < hw ? ix + 1 : ix;
            tx = fx - (float)ix;
            if (tx < 0.0f) tx = 0.0f; else if (tx > 1.0f) tx = 1.0f;

            a = half + ((size_t)iy * hw + ix) * 4;
            b = half + ((size_t)iy * hw + ix1) * 4;
            cc = half + ((size_t)iy1 * hw + ix) * 4;
            d = half + ((size_t)iy1 * hw + ix1) * 4;
            w00 = (1.0f - tx) * (1.0f - ty);
            w01 = tx * (1.0f - ty);
            w10 = (1.0f - tx) * ty;
            w11 = tx * ty;
            for (k = 0; k < 4; k++)
            {
                float v = a[k] * w00 + b[k] * w01 + cc[k] * w10 + d[k] * w11;
                o[k] = (BYTE)(v + 0.5f);
            }
        }
    }

    free(half);
    out->w = src->w;
    out->h = src->h;
    out->px = up;
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
        blur_bgra(g_ltp.px, g_ltp.w, g_ltp.h, r / (float)g_ltp.scale);
        g_ltp.radius = r;
        InterlockedIncrement(&g_gen);
    }
    LeaveCriticalSection(&g_live_lock);
    return TRUE;
}

void glass_set_live_idle(BOOL on)
{
    live_init();
    InterlockedExchange(&g_live_idle, on ? 1 : 0);
    /* Toggles the capture exclusion (and the burst cadence) with it. */
    live_fast_sync();
    if (g_wake)
        SetEvent(g_wake);
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

/* Starts a fresh tile build without dropping the current one: a press is
 * about to turn into a drag, and the first drag frames must not paint the
 * tile from before the press. */
void glass_live_warm(void)
{
    live_init();
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

/* Age of the freshest backdrop the card would paint right now (worker tile or
 * frame-exact drag capture), in ms; -1 if none has ever been produced. */
LONG glass_live_age_ms(void)
{
    LONG t = InterlockedCompareExchange(&g_plane_tick, 0, 0);
    LONG s = InterlockedCompareExchange(&g_sync_tick, 0, 0);
    if (s > t)
        t = s;
    return t ? (LONG)(GetTickCount64() - (ULONGLONG)t) : -1;
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
