/* RecycleGlass-C — main window, tray, menu, input and worker threads. */
#include "common.h"
#include "config.h"
#include "i18n.h"
#include "render.h"
#include "glass.h"
#include "shell.h"
#include "droptarget.h"
#include "menu.h"
#include "resource.h"

#define POLL_MS        5000
#define HOVER_DUR      0.16f
#define FX_DUR         0.42f
#define DRAG_THRESHOLD 4
#define TOAST_MS       2600

static HINSTANCE g_inst;
static HWND      g_hwnd;
static Config    g_cfg;
static RgSurface g_surf;
static MenuHost  g_menu_host;

static RgBitmap  g_backdrop;
/* Pristine copy of the last real capture.  Sliding must always start from this,
 * otherwise every frame smears the already-smeared image and a fast drag leaves
 * permanent streaks. */
/* TRUE when the current backdrop came from a real capture rather than the
 * wallpaper fallback.  While it is FALSE we keep retrying the live crop, so a
 * capture that lands later still replaces the wallpaper. */
static BOOL      g_backdrop_live;
static BOOL      g_backdrop_valid;
static int       g_backdrop_x, g_backdrop_y;
static float     g_backdrop_blur = -1.0f;
/* Id of the live frame the current backdrop was built from.  Without this the
 * early-out below treats "same position, same radius" as "nothing to do", so
 * the card kept the very first capture and never showed the desktop again. */
static LONG      g_backdrop_gen = -1;

static long long g_count, g_size;
static BOOL      g_hover, g_drag_hover;
static float     g_btn_t[2];        /* eased pill highlight */
static int       g_hover_btn;
static float     g_hover_t;
static int       g_hover_target;
static ULONGLONG g_hover_t0;
static BOOL      g_hover_ticking;

static float     g_fx_t = 1.0f;
static BOOL      g_fx_active;
static ULONGLONG g_fx_t0;

static BOOL      g_dragging, g_btn_down;
static int       g_drag_last_x, g_drag_last_y;
static ULONGLONG g_last_frame_us;
static BOOL      g_live_backdrop;   /* blur the real desktop, not the wallpaper */
static POINT     g_drag_cursor0, g_drag_win0, g_mouse_down;
static BOOL      g_tracking;

static wchar_t   g_toast[256];
static ULONGLONG g_toast_until;

static BOOL      g_busy;
static ULONGLONG g_busy_t0;

static BOOL      g_running = TRUE;
static BOOL      g_cfg_dirty;

static NOTIFYICONDATAW g_nid;
static HICON     g_tray_icon;
static UINT      g_taskbar_msg;

static CRITICAL_SECTION g_lock;
static long long g_q_count, g_q_size;
static BOOL      g_op_ok;
static wchar_t   g_op_msg[256];

static HANDLE    g_wake, g_stop_evt, g_poll_thread;

/* --------------------------------------------------------------- helpers */

static ULONGLONG now_ms(void)
{
    return GetTickCount64();
}

/* Microsecond clock.  GetTickCount64() ticks every ~15.6 ms, which is useless
 * for pacing frames on a high-refresh display. */
static ULONGLONG now_us(void)
{
    static ULONGLONG freq;
    LARGE_INTEGER c;

    if (!freq)
        freq = (ULONGLONG)1;
    if (freq == 1)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (ULONGLONG)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    /* Integer maths on purpose: going through a double loses ~2 ms of
     * resolution at QPC magnitudes, which turns a 4 ms frame budget into an
     * uneven 2/4 ms cadence - visible as judder on a 240 Hz panel. */
    return (ULONGLONG)c.QuadPart * 1000000ULL / freq;
}

/* ---------------------------------------------------------------- profile
 *
 * Set RG_PROF=1 to append one summary line per 90 painted frames to
 * %LOCALAPPDATA%\RecycleGlass\perf.log.  Everything here is a few adds per
 * frame, so the instrumented build measures its own overhead and nothing else.
 */
static int g_prof = -1;
static unsigned g_frame_us = 16667;

typedef struct {
    unsigned  n;
    ULONGLONG tot, max;
    ULONGLONG bd, rend, ulw;
    unsigned  hit, miss, wp;
    ULONGLONG follow_us, live_us;
    ULONGLONG rs_us, sub_us;
    ULONGLONG eg_us, ee_us, eb_us;
    unsigned  iv_n, iv_slow, n_moves, n_still;
    ULONGLONG iv_sum, iv_max;
} Prof;

static Prof g_pf;

static int prof_on(void)
{
    if (g_prof < 0)
    {
        wchar_t v[8] = { 0 };
        g_prof = (GetEnvironmentVariableW(L"RG_PROF", v, RG_COUNT(v)) > 0 &&
                  v[0] == L'1');
    }
    return g_prof;
}

static void prof_flush(void)
{
    static wchar_t path[MAX_PATH];
    wchar_t base[MAX_PATH];
    FILE *f;

    if (!path[0])
    {
        GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        swprintf_s(path, MAX_PATH, L"%ls\\%ls\\perf.log", base, RG_APP_NAME);
    }
    f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f)
        return;
    fwprintf(f, L"n=%u avg=%.2f max=%.2f bd=%.2f rend=%.2f ulw=%.2f hit=%u miss=%u wp=%u fol=%.2f liv=%.2f rs=%.2f sub=%.2f eg=%.2f ee=%.2f eb=%.2f iv=%.2f ivmax=%.2f slow=%u mv=%u st=%u\n",
             g_pf.n,
             (double)(g_pf.tot / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)g_pf.max / 1000.0,
             (double)(g_pf.bd / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.rend / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.ulw / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             g_pf.hit, g_pf.miss, g_pf.wp,
             (double)(g_pf.follow_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.live_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.rs_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.sub_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.eg_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.ee_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.eb_us / (g_pf.n ? g_pf.n : 1)) / 1000.0,
             (double)(g_pf.iv_sum / (g_pf.iv_n ? g_pf.iv_n : 1)) / 1000.0,
             (double)g_pf.iv_max / 1000.0,
             g_pf.iv_slow, g_pf.n_moves, g_pf.n_still);
    fclose(f);
    memset(&g_pf, 0, sizeof(g_pf));
}

static void prof_frame(ULONGLONG bd, ULONGLONG rend, ULONGLONG ulw, ULONGLONG tot)
{
    static ULONGLONG prev;
    ULONGLONG now;

    if (!prof_on())
        return;

    if (!g_dragging)
    {
        prev = 0;
        memset(&g_pf, 0, sizeof(g_pf));
        return;
    }

    now = now_us();
    if (prev)
    {
        ULONGLONG iv = now - prev;
        g_pf.iv_n++;
        g_pf.iv_sum += iv;
        if (iv > g_pf.iv_max)
            g_pf.iv_max = iv;
        if (iv > (ULONGLONG)(g_frame_us * 3 / 2))
            g_pf.iv_slow++;
    }
    prev = now;
    g_pf.n++;
    g_pf.tot += tot;
    g_pf.bd += bd;
    g_pf.rend += rend;
    g_pf.ulw += ulw;
    if (tot > g_pf.max)
        g_pf.max = tot;
    if (g_pf.n >= 90)
        prof_flush();
}

/* Display refresh period in microseconds (defaults to 60 Hz). */
/* One frame per display refresh.  A 1 kHz mouse sends a move every millisecond;
 * repainting on each one costs 2.2 ms, i.e. 220% of a core, which saturates the
 * UI thread and makes the whole drag stutter.  Rendering at the panel rate and
 * always reading the *newest* cursor position is both cheaper and smoother. */
static unsigned g_drag_frame_us = 4166;

static void detect_refresh(void)
{
    DEVMODEW dm;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500)
        g_frame_us = 1000000u / dm.dmDisplayFrequency;

    g_drag_frame_us = g_frame_us;
}

/* Tiny rotating-free logger: %LOCALAPPDATA%\RecycleGlass\recycleglass.log.
 * The Python original had a whole diag module for this; a single append-only
 * file is enough to answer "why did it not show up?". */
static void diag(const wchar_t *fmt, ...)
{
    static wchar_t path[MAX_PATH];
    wchar_t line[1024];
    wchar_t stamp[64];
    SYSTEMTIME st;
    va_list ap;
    FILE *f;

    if (!path[0])
    {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            wcscpy_s(base, MAX_PATH, L".");
        swprintf_s(path, MAX_PATH, L"%ls\\%ls", base, RG_APP_NAME);
        CreateDirectoryW(path, NULL);
        swprintf_s(path, MAX_PATH, L"%ls\\%ls\\recycleglass.log", base, RG_APP_NAME);
    }

    GetLocalTime(&st);
    swprintf_s(stamp, RG_COUNT(stamp), L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);

    va_start(ap, fmt);
    _vsnwprintf_s(line, RG_COUNT(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f)
        return;
    fwprintf(f, L"%ls  %ls\n", stamp, line);
    fclose(f);
}

static BOOL system_is_light(void)
{
    static ULONGLONG cached_at;
    static BOOL cached = TRUE;
    ULONGLONG now = now_ms();

    if (now - cached_at < 2000)
        return cached;

    {
        HKEY key;
        DWORD val = 1, size = sizeof(val), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type,
                                 (LPBYTE)&val, &size) != ERROR_SUCCESS)
                val = 1;
            RegCloseKey(key);
        }
        cached = val ? TRUE : FALSE;
    }

    cached_at = now;
    return cached;
}

static BOOL is_dark(void)
{
    if (wcscmp(g_cfg.theme, L"dark") == 0)
        return TRUE;
    if (wcscmp(g_cfg.theme, L"light") == 0)
        return FALSE;
    return !system_is_light();
}

static float compute_scale(void)
{
    float s = 1.0f;

    if (wcscmp(g_cfg.size, L"small") == 0)
        s = 0.85f;
    else if (wcscmp(g_cfg.size, L"large") == 0)
        s = 1.18f;

    if (g_cfg.auto_dpi)
    {
        UINT dpi = 96;
        float f;
        if (g_hwnd)
            dpi = GetDpiForWindow(g_hwnd);
        if (!dpi)
            dpi = 96;
        /* Damped: at 100% nothing changes, but a 150% desktop only grows the
         * widget to 112.5% instead of 150% - full system scaling makes the card
         * look oversized on high-DPI laptops. */
        f = (float)dpi / 96.0f;
        s *= 0.75f + 0.25f * f;
    }

    return rg_clampf(s, 0.6f, 2.5f);
}

static void work_area_at(int x, int y, RECT *out)
{
    POINT pt;
    HMONITOR mon;
    MONITORINFO mi;

    pt.x = x;
    pt.y = y;
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);

    if (mon && GetMonitorInfoW(mon, &mi))
        *out = mi.rcWork;
    else
    {
        out->left = 0;
        out->top = 0;
        out->right = GetSystemMetrics(SM_CXSCREEN);
        out->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static void clamp_position(int *x, int *y, int w, int h)
{
    RECT wa;
    work_area_at(*x + w / 2, *y + h / 2, &wa);
    *x = rg_clampi(*x, wa.left, wa.right - w);
    *y = rg_clampi(*y, wa.top, wa.bottom - h);
}

/* ------------------------------------------------------------ config save */

static void save_cfg(void)
{
    RECT r;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &r))
        return;

    g_cfg.x = r.left;
    g_cfg.y = r.top;
    g_cfg.width = r.right - r.left;
    g_cfg.height = r.bottom - r.top;
    g_cfg.last_count = g_count;
    g_cfg.last_size = g_size;

    config_save(&g_cfg);
    g_cfg_dirty = FALSE;
}

/* --------------------------------------------------------------- backdrop */

/* Window origin for the frame currently being built.  While dragging, the move
 * and the content upload are folded into one UpdateLayeredWindow call: two
 * calls (SetWindowPos + ULW) mean two compositor updates per frame, which is
 * exactly what makes a drag feel like it lags behind the pointer. */
static POINT g_frame_pos;
static BOOL  g_frame_moves;

static void invalidate_backdrop(void)
{
    glass_live_invalidate();
    glass_release(&g_backdrop);
    g_backdrop_valid = FALSE;
    g_backdrop_blur = -1.0f;
    g_backdrop_live = FALSE;
}

/* Opts the widget out of screen capture so its own pixels never end up in the
 * backdrop it is about to blur. */
static void init_capture(void)
{
    g_live_backdrop = FALSE;

    if (wcscmp(g_cfg.blur_mode, L"wallpaper") == 0)
    {
        /* Blurring the wallpaper needs no capture at all, and dropping the
         * capture exclusion makes the widget show up in screenshots again. */
        SetWindowDisplayAffinity(g_hwnd, 0 /* WDA_NONE */);
        return;
    }
    if (!glass_live_available())
        return;

    /* WDA_EXCLUDEFROMCAPTURE: the window is invisible to BitBlt of the desktop. */
    if (!SetWindowDisplayAffinity(g_hwnd, 0x00000011))
        return;

    g_live_backdrop = TRUE;
}

static void ensure_backdrop(void)
{
    RECT r;
    RgBitmap fresh;
    int bx, by, bw, bh;

    ULONGLONG p0, p1, p2;

    if (!g_hwnd)
        return;

    p0 = now_us();
    if (g_frame_moves)
    {
        r.left = g_frame_pos.x;
        r.top = g_frame_pos.y;
    }
    else if (!GetWindowRect(g_hwnd, &r))
        return;

    bx = r.left + render_pad();
    by = r.top + render_pad();
    bw = render_card_w();
    bh = render_card_h();

    p1 = now_us();
    if (g_backdrop_valid &&
        g_backdrop_x == bx && g_backdrop_y == by &&
        g_backdrop.w == bw && g_backdrop.h == bh &&
        g_backdrop_blur == g_cfg.blur_radius &&
        (!g_live_backdrop || (g_backdrop_live && g_backdrop_gen == glass_live_gen())))
        return;

    if (g_live_backdrop)
    {
        ULONGLONG f0 = now_us();
        glass_live_follow(bx, by, bw, bh, g_cfg.blur_radius, g_dragging);
        g_pf.follow_us += now_us() - f0;
    }
    else
        g_pf.miss++;

    p2 = now_us();
    g_pf.eg_us += p1 - p0;
    g_pf.ee_us += p2 - p1;

    memset(&fresh, 0, sizeof(fresh));

    /* While the live backdrop is on, a miss means "a capture is on its way" -
     * keep painting the previous one instead of flashing the wallpaper. */
    if (g_live_backdrop)
    {
        ULONGLONG l0 = now_us();
        BOOL lhit = glass_live(bx, by, bw, bh, g_cfg.blur_radius, &fresh);
        g_pf.live_us += now_us() - l0;

        if (lhit)
        {
            g_pf.hit++;
            glass_release(&g_backdrop);
            g_backdrop = fresh;
            g_backdrop_valid = TRUE;
            g_backdrop_x = bx;
            g_backdrop_y = by;
            g_backdrop_blur = g_cfg.blur_radius;
            g_backdrop_live = TRUE;
            g_backdrop_gen = glass_live_gen();
        }
        /* No slide-to-follow any more: sliding a still frame to the new window
         * position leaves a strip of duplicated edge rows along the leading
         * side, which reads as a hard line plus horizontal streaks.  A fresh
         * capture lands within a frame now, so the old one is simply kept. */
        else if (!g_backdrop_valid &&
                 glass_backdrop(bx, by, bw, bh, g_cfg.blur_radius, &fresh))
        {
            g_pf.wp++;
            /* Nothing captured yet (first frame, or the desktop is not
             * capturable at all): start from the wallpaper. */
            g_backdrop = fresh;
            g_backdrop_valid = TRUE;
            g_backdrop_x = bx;
            g_backdrop_y = by;
            g_backdrop_blur = g_cfg.blur_radius;
            g_backdrop_live = FALSE;
            g_backdrop_gen = glass_live_gen();
        }
        g_pf.eb_us += now_us() - p2;
        return;
    }

    if (glass_backdrop(bx, by, bw, bh, g_cfg.blur_radius, &fresh))
    {
        g_pf.wp++;
        glass_release(&g_backdrop);
        g_backdrop = fresh;
        g_backdrop_valid = TRUE;
        g_backdrop_x = bx;
        g_backdrop_y = by;
        g_backdrop_blur = g_cfg.blur_radius;
    }
}

/* ---------------------------------------------------------------- subtitle */

static void subtitle_text(wchar_t *out, size_t cap)
{
    if (g_busy)
    {
        long long secs = (long long)((now_ms() - g_busy_t0) / 1000);
        swprintf_s(out, cap, T(S_BUSY_EMPTY), secs);
        return;
    }
    if (g_drag_hover)
    {
        wcscpy_s(out, cap, T(S_DROP_HINT));
        return;
    }
    if (g_toast[0] && now_ms() < g_toast_until)
    {
        wcscpy_s(out, cap, g_toast);
        return;
    }
    if (g_count > 0)
    {
        wchar_t sz[64];
        shell_format_size(g_size, sz, RG_COUNT(sz));
        swprintf_s(out, cap, T(S_SUB_COUNT), g_count, sz);
        return;
    }
    wcscpy_s(out, cap, T(S_SUB_EMPTY));
}

/* ------------------------------------------------------------------ toast */

static void show_toast(const wchar_t *text)
{
    if (!text || !*text)
        return;
    wcsncpy_s(g_toast, RG_COUNT(g_toast), text, _TRUNCATE);
    g_toast_until = now_ms() + TOAST_MS;
    if (g_hwnd)
        SetTimer(g_hwnd, RG_TIMER_TOAST, 120, NULL);
}

/* ------------------------------------------------------------------ paint */

/* Debug aid: the window opts out of screen capture, so PrintWindow and friends
 * return black.  This writes the composited surface itself to a PNG instead. */
static void dump_frame(void)
{
    GpBitmap *bmp = NULL;
    CLSID clsid;
    UINT num = 0, size = 0, i;
    ImageCodecInfo *info;
    wchar_t path[MAX_PATH];

    if (!g_surf.bits || !g_surf.w || !g_surf.h)
        return;

    if (GdipCreateBitmapFromScan0(g_surf.w, g_surf.h, g_surf.w * 4,
                                  PixelFormat32bppPARGB, g_surf.bits, &bmp) != Ok)
        return;

    GdipGetImageEncodersSize(&num, &size);
    info = size ? (ImageCodecInfo *)malloc(size) : NULL;
    if (info)
    {
        GdipGetImageEncoders(num, size, info);
        for (i = 0; i < num; i++)
        {
            if (wcscmp(info[i].MimeType, L"image/png") == 0)
            {
                clsid = info[i].Clsid;
                GetTempPathW(MAX_PATH, path);
                wcscat_s(path, MAX_PATH, L"rg_frame.png");
                GdipSaveImageToFile((GpImage *)bmp, path, &clsid, NULL);
                break;
            }
        }
        free(info);
    }
    GdipDisposeImage(bmp);

}

static void refresh(void)
{
    RgRenderState st;
    wchar_t sub[256];
    SIZE size;
    POINT src, dst;
    POINT *pdst = NULL;
    BLENDFUNCTION bf;
    BOOL prof = prof_on();
    ULONGLONG t0 = 0, t1 = 0, t2 = 0, tA = 0, tB = 0;

    if (!g_hwnd)
        return;

    if (prof)
    {
        t0 = now_us();
        if (g_frame_moves)
            g_pf.n_moves++;
        else
            g_pf.n_still++;
    }

    if (!render_surface_resize(&g_surf, render_win_w(), render_win_h()))
        return;

    if (prof)
        tA = now_us();

    ensure_backdrop();

    if (prof)
        tB = now_us();

    subtitle_text(sub, RG_COUNT(sub));

    if (prof)
        t1 = now_us();

    memset(&st, 0, sizeof(st));
    st.backdrop = g_backdrop_valid ? &g_backdrop : NULL;
    st.backdrop_x = g_backdrop_x;
    st.backdrop_y = g_backdrop_y;
    st.backdrop_gen = (unsigned)g_backdrop_gen;
    st.blur = g_cfg.blur_radius;
    st.title = T(S_APP_NAME);
    st.subtitle = sub;
    st.hover = g_hover;
    st.hover_btn = g_hover_btn;
    st.btn_t[0] = g_btn_t[0];
    st.btn_t[1] = g_btn_t[1];
    st.hover_t = g_hover_t;
    st.opacity = g_cfg.opacity;
    st.material = g_cfg.material;
    st.drag_hover = g_drag_hover;
    st.has_fx = g_fx_active;
    st.fx_t = g_fx_t;
    st.dark = is_dark();

    render_card(&g_surf, &st);

    if (prof)
        t2 = now_us();

    size.cx = render_win_w();
    size.cy = render_win_h();
    src.x = 0;
    src.y = 0;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    if (g_frame_moves)
    {
        dst.x = g_frame_pos.x;
        dst.y = g_frame_pos.y;
        pdst = &dst;
        g_frame_moves = FALSE;
    }

    if (!UpdateLayeredWindow(g_hwnd, NULL, pdst, &size, g_surf.dc, &src, 0,
                             &bf, ULW_ALPHA))
    {
        static BOOL logged;
        if (!logged)
        {
            logged = TRUE;
            diag(L"UpdateLayeredWindow failed err=%lu", GetLastError());
        }
    }

    if (prof)
    {
        ULONGLONG t3 = now_us();
        g_pf.rs_us += tA - t0;
        g_pf.sub_us += t1 - tB;
        prof_frame(t1 - t0, t2 - t1, t3 - t2, t3 - t0);
    }
}

/* One drag frame, driven by the mouse stream and capped at the display refresh
 * rate.  A WM_TIMER driven loop was tried first: WM_TIMER is the lowest
 * priority message, so a 1 kHz mouse starves it and the drag falls behind. */
static BOOL drag_frame(void)
{
    POINT cur;
    int tx, ty;
    ULONGLONG now;

    if (!g_dragging || !g_hwnd)
        return FALSE;

    now = now_us();
    if (g_drag_frame_us && now - g_last_frame_us < g_drag_frame_us)
        return FALSE;
    g_last_frame_us = now;

    GetCursorPos(&cur);
    tx = g_drag_win0.x + (cur.x - g_drag_cursor0.x);
    ty = g_drag_win0.y + (cur.y - g_drag_cursor0.y);
    if (tx == g_drag_last_x && ty == g_drag_last_y)
        return FALSE;

    g_drag_last_x = tx;
    g_drag_last_y = ty;

    g_frame_pos.x = tx;
    g_frame_pos.y = ty;
    g_frame_moves = TRUE;
    refresh();
    return TRUE;
}

/* Paints the next frame at (x, y) and moves the window there in one step. */
static void refresh_at(int x, int y)
{
    g_frame_pos.x = x;
    g_frame_pos.y = y;
    g_frame_moves = TRUE;
    refresh();
}

/* ------------------------------------------------------------ animations */

/* The card fade and the pill highlights share one timer, so anything that
 * changes what should be lit has to make sure it is running.  Without this a
 * pointer that slides onto a pill *after* the fade has settled lights nothing
 * up - the timer had already stopped. */
static void kick_hover(void)
{
    if (!g_hover_ticking)
    {
        g_hover_ticking = TRUE;
        SetTimer(g_hwnd, RG_TIMER_HOVER, 16, NULL);
    }
}

static void start_hover_anim(int target)
{
    float from;

    if (g_hover_target == target)
        return;

    g_hover_target = target;

    /* Re-base the clock on the current progress so a reversal mid-flight
     * continues from where the animation actually is. */
    from = target ? g_hover_t : (1.0f - g_hover_t);
    g_hover_t0 = now_ms() - (ULONGLONG)(from * HOVER_DUR * 1000.0f);

    kick_hover();
}

static void tick_hover(void)
{
    float t = (float)(now_ms() - g_hover_t0) / (HOVER_DUR * 1000.0f);
    BOOL settled = TRUE;
    int i;

    if (t >= 1.0f)
    {
        g_hover_t = (float)g_hover_target;
    }
    else
    {
        g_hover_t = rg_clampf(g_hover_target ? t : 1.0f - t, 0.0f, 1.0f);
        settled = FALSE;
    }

    /* Pill highlights ease towards their target too, so moving between the two
     * buttons cross-fades instead of snapping. */
    for (i = 0; i < 2; i++)
    {
        float target = (g_hover_btn == i + 1) ? 1.0f : 0.0f;
        float d = target - g_btn_t[i];
        if (fabsf(d) < 0.02f)
        {
            g_btn_t[i] = target;
        }
        else
        {
            g_btn_t[i] += d * 0.34f;
            settled = FALSE;
        }
    }

    if (settled)
    {
        g_hover_ticking = FALSE;
        KillTimer(g_hwnd, RG_TIMER_HOVER);
    }

    refresh();
}

static void start_fx(void)
{
    g_fx_active = TRUE;
    g_fx_t = 0.0f;
    g_fx_t0 = now_ms();
    SetTimer(g_hwnd, RG_TIMER_FX, 16, NULL);
}

static void tick_fx(void)
{
    float t = (float)(now_ms() - g_fx_t0) / (FX_DUR * 1000.0f);

    if (t >= 1.0f)
    {
        g_fx_active = FALSE;
        g_fx_t = 1.0f;
        KillTimer(g_hwnd, RG_TIMER_FX);
    }
    else
    {
        g_fx_t = t;
    }
    refresh();
}

/* ------------------------------------------------------------------- tray */

static void update_tray_tip(void)
{
    wchar_t sz[64];

    if (!g_nid.cbSize)
        return;

    if (g_count > 0)
    {
        shell_format_size(g_size, sz, RG_COUNT(sz));
        swprintf_s(g_nid.szTip, RG_COUNT(g_nid.szTip), T(S_TRAY_COUNT), g_count, sz);
    }
    else
    {
        wcscpy_s(g_nid.szTip, RG_COUNT(g_nid.szTip), T(S_TRAY_EMPTY));
    }

    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void init_tray(void)
{
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);

    if (!g_tray_icon)
        g_tray_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_TRAY),
                                        IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!g_tray_icon)
        g_tray_icon = LoadIconW(NULL, IDI_APPLICATION);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = RG_WM_TRAY;
    g_nid.hIcon = g_tray_icon;
    wcscpy_s(g_nid.szTip, RG_COUNT(g_nid.szTip), RG_APP_NAME);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    update_tray_tip();
}

static void remove_tray(void)
{
    if (g_nid.cbSize)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        memset(&g_nid, 0, sizeof(g_nid));
    }
}

/* ------------------------------------------------------------- operations */

typedef struct {
    int   kind;      /* 0 = empty, 1 = recycle */
    int   n;
    wchar_t **paths;
} OpJob;

static DWORD WINAPI op_thread(LPVOID param)
{
    OpJob *job = (OpJob *)param;
    BOOL ok = FALSE;
    wchar_t msg[256] = L"";
    int i;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (job->kind == 0)
    {
        ok = shell_empty(FALSE);
        if (ok)
            wcscpy_s(msg, RG_COUNT(msg), T(S_TOAST_EMPTY_DONE));
        else
            wcscpy_s(msg, RG_COUNT(msg), T(S_TOAST_FAILED));
    }
    else
    {
        ok = shell_recycle_paths(job->paths, job->n, msg, RG_COUNT(msg));
    }

    EnterCriticalSection(&g_lock);
    g_op_ok = ok;
    wcsncpy_s(g_op_msg, RG_COUNT(g_op_msg), msg, _TRUNCATE);
    LeaveCriticalSection(&g_lock);

    if (g_hwnd)
        PostMessage(g_hwnd, RG_WM_OP, 0, 0);

    for (i = 0; i < job->n; i++)
        free(job->paths[i]);
    free(job->paths);
    free(job);

    CoUninitialize();
    return 0;
}

static void start_empty(void)
{
    OpJob *job;

    if (g_busy)
        return;

    job = (OpJob *)calloc(1, sizeof(*job));
    if (!job)
        return;
    job->kind = 0;

    g_busy = TRUE;
    g_busy_t0 = now_ms();
    SetTimer(g_hwnd, RG_TIMER_BUSY, 500, NULL);
    refresh();

    {
        HANDLE th = CreateThread(NULL, 0, op_thread, job, 0, NULL);
        if (th)
            CloseHandle(th);
        else
        {
            g_busy = FALSE;
            free(job);
        }
    }
}

static void start_recycle(wchar_t **paths, int n)
{
    OpJob *job;
    int i;

    if (n <= 0)
        return;

    job = (OpJob *)calloc(1, sizeof(*job));
    if (!job)
        return;

    job->kind = 1;
    job->n = n;
    job->paths = (wchar_t **)calloc((size_t)n, sizeof(wchar_t *));
    if (!job->paths)
    {
        free(job);
        return;
    }

    for (i = 0; i < n; i++)
    {
        size_t len = wcslen(paths[i]) + 1;
        job->paths[i] = (wchar_t *)calloc(len, sizeof(wchar_t));
        if (job->paths[i])
            wcscpy_s(job->paths[i], len, paths[i]);
    }

    g_busy = TRUE;
    g_busy_t0 = now_ms();
    SetTimer(g_hwnd, RG_TIMER_BUSY, 500, NULL);

    {
        HANDLE th = CreateThread(NULL, 0, op_thread, job, 0, NULL);
        if (th)
            CloseHandle(th);
        else
            g_busy = FALSE;
    }
}

static void request_query(void)
{
    if (g_wake)
        SetEvent(g_wake);
}

/* --------------------------------------------------------------- poller */

static DWORD WINAPI poll_thread(LPVOID param)
{
    (void)param;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    for (;;)
    {
        HANDLE waits[2];
        DWORD  hit;

        waits[0] = g_wake;
        waits[1] = g_stop_evt;
        hit = WaitForMultipleObjects(2, waits, FALSE, POLL_MS);

        if (hit == WAIT_OBJECT_0 + 1)
            break;

        {
            long long c = 0, s = 0;
            shell_query(&c, &s);
            EnterCriticalSection(&g_lock);
            g_q_count = c;
            g_q_size = s;
            LeaveCriticalSection(&g_lock);
            if (g_hwnd)
                PostMessage(g_hwnd, RG_WM_BIN, 0, 0);
        }
    }

    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------- menu */

static const int BLUR_STEPS[]     = { 0, 12, 24, 36, 48 };
static const int OPACITY_STEPS[]  = { 60, 70, 78, 85, 95 };
static const int MATERIAL_STEPS[] = { 30, 55, 75, 100 };

static void apply_appearance(void)
{
    render_set_scale(compute_scale());
    invalidate_backdrop();
    render_invalidate();
    refresh();
    g_cfg_dirty = TRUE;
}

static void apply_zorder(void)
{
    SetWindowPos(g_hwnd, g_cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void apply_autostart(void)
{
    HKEY key;
    wchar_t exe[MAX_PATH];
    wchar_t cmd[MAX_PATH + 8];
    LONG rc;

    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;

    if (g_cfg.autostart)
    {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf_s(cmd, RG_COUNT(cmd), L"\"%ls\"", exe);
        rc = RegSetValueExW(key, L"RecycleGlass", 0, REG_SZ,
                            (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    }
    else
    {
        rc = RegDeleteValueW(key, L"RecycleGlass");
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
    }
    (void)rc;
    RegCloseKey(key);
}

static void handle_command(int cmd)
{
    switch (cmd)
    {
    case RG_CMD_OPEN:
        shell_open_bin();
        break;

    case RG_CMD_EMPTY:
        if (g_count == 0)
        {
            show_toast(T(S_TOAST_ALREADY_EMPTY));
            refresh();
        }
        else if (g_cfg.confirm_empty)
        {
            /* Same glass panel as the menu, in its dialog form - a native
             * MessageBox here looked like a different application. */
            POINT pt;
            GetCursorPos(&pt);
            menu_confirm_show(pt.x, pt.y);
        }
        else
        {
            start_empty();
        }
        break;

    case RG_CMD_EMPTY_NOW:
        if (g_count > 0)
            start_empty();
        break;

    case RG_CMD_AUTOSTART:
        g_cfg.autostart = !g_cfg.autostart;
        apply_autostart();
        g_cfg_dirty = TRUE;
        break;

    case RG_CMD_BLUR_SCREEN:
    case RG_CMD_BLUR_WALLPAPER:
        wcscpy_s(g_cfg.blur_mode, RG_COUNT(g_cfg.blur_mode),
                 cmd == RG_CMD_BLUR_WALLPAPER ? L"wallpaper" : L"screen");
        init_capture();
        if (g_live_backdrop)
            glass_live_set_target(g_hwnd);
        invalidate_backdrop();
        render_invalidate();
        refresh();
        g_cfg_dirty = TRUE;
        break;

    case RG_CMD_DPI:
        g_cfg.auto_dpi = !g_cfg.auto_dpi;
        apply_appearance();
        break;

    case RG_CMD_TOPMOST:
        g_cfg.topmost = !g_cfg.topmost;
        apply_zorder();
        g_cfg_dirty = TRUE;
        break;

    case RG_CMD_EXIT:
        g_running = FALSE;
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        break;

    case RG_CMD_THEME_SYSTEM:
        wcscpy_s(g_cfg.theme, RG_COUNT(g_cfg.theme), L"system");
        invalidate_backdrop();
        render_invalidate();
        refresh();
        g_cfg_dirty = TRUE;
        break;
    case RG_CMD_THEME_LIGHT:
        wcscpy_s(g_cfg.theme, RG_COUNT(g_cfg.theme), L"light");
        invalidate_backdrop();
        render_invalidate();
        refresh();
        g_cfg_dirty = TRUE;
        break;
    case RG_CMD_THEME_DARK:
        wcscpy_s(g_cfg.theme, RG_COUNT(g_cfg.theme), L"dark");
        invalidate_backdrop();
        render_invalidate();
        refresh();
        g_cfg_dirty = TRUE;
        break;

    case RG_CMD_SIZE_SMALL:
    case RG_CMD_SIZE_NORMAL:
    case RG_CMD_SIZE_LARGE:
        wcscpy_s(g_cfg.size, RG_COUNT(g_cfg.size),
                 cmd == RG_CMD_SIZE_SMALL ? L"small" :
                 (cmd == RG_CMD_SIZE_LARGE ? L"large" : L"normal"));
        apply_appearance();
        break;

    case RG_CMD_RESET:
        g_cfg.opacity = 0.78f;
        g_cfg.material = 0.55f;
        g_cfg.blur_radius = 24.0f;
        wcscpy_s(g_cfg.theme, RG_COUNT(g_cfg.theme), L"system");
        wcscpy_s(g_cfg.size, RG_COUNT(g_cfg.size), L"normal");
        apply_appearance();
        break;

    default:
        if (cmd >= RG_CMD_BLUR_BASE && cmd < RG_CMD_BLUR_BASE + RG_COUNT(BLUR_STEPS))
        {
            g_cfg.blur_radius = (float)BLUR_STEPS[cmd - RG_CMD_BLUR_BASE];
            invalidate_backdrop();
            /* Re-blur what we already captured instead of grabbing the screen
             * again - the card updates instantly and nothing stalls. */
            glass_live_reblur(g_cfg.blur_radius);
            render_invalidate();
            refresh();
            g_cfg_dirty = TRUE;
        }
        else if (cmd >= RG_CMD_OPACITY_BASE && cmd < RG_CMD_OPACITY_BASE + RG_COUNT(OPACITY_STEPS))
        {
            g_cfg.opacity = (float)OPACITY_STEPS[cmd - RG_CMD_OPACITY_BASE] / 100.0f;
            refresh();
            g_cfg_dirty = TRUE;
        }
        else if (cmd >= RG_CMD_MATERIAL_BASE && cmd < RG_CMD_MATERIAL_BASE + RG_COUNT(MATERIAL_STEPS))
        {
            g_cfg.material = (float)MATERIAL_STEPS[cmd - RG_CMD_MATERIAL_BASE] / 100.0f;
            refresh();
            g_cfg_dirty = TRUE;
        }
        break;
    }
}

static void menu_command(int cmd)
{
    handle_command(cmd);
}

static int menu_query_dark(void)
{
    return is_dark();
}

static float menu_query_scale(void)
{
    return render_scale();
}

/* Live slider feedback from the menu: values arrive already quantised. */
/* Slider drag preview.  Repainting the card on *every* step saturated the UI
 * thread (the slider sends 500+ updates a second); repainting it never made the
 * drag feel dead.  20 fps is the middle ground - the card visibly follows the
 * slider, and each repaint costs 2 ms instead of 500 of them a second. */
/* While a slider is being dragged this runs on every mouse move, so it must
 * stay allocation- and repaint-free: the menu repaints itself from cfg right
 * after this returns, and the card is behind the menu anyway.  Everything
 * expensive (re-blur, card repaint) waits for menu_set() at the drag end. */
static void menu_preview(int id, float v)
{
    switch (id)
    {
    case RG_SET_BLUR:
        g_cfg.blur_radius = rg_clampf(v, 0.0f, 48.0f);
        g_cfg_dirty = TRUE;
        break;
    case RG_SET_OPACITY:
        g_cfg.opacity = rg_clampf(v, 0.2f, 1.0f);
        g_cfg_dirty = TRUE;
        break;
    case RG_SET_MATERIAL:
        g_cfg.material = rg_clampf(v, 0.0f, 1.0f);
        g_cfg_dirty = TRUE;
        break;
    default:
        break;
    }
}

static void menu_set(int id, float v)
{
    switch (id)
    {
    case RG_SET_OPACITY:
        g_cfg.opacity = rg_clampf(v, 0.30f, 1.00f);
        refresh();
        g_cfg_dirty = TRUE;
        break;

    case RG_SET_MATERIAL:
        g_cfg.material = rg_clampf(v, 0.0f, 1.0f);
        refresh();
        g_cfg_dirty = TRUE;
        break;

    case RG_SET_BLUR:
        g_cfg.blur_radius = rg_clampf(v, 0.0f, 48.0f);
        invalidate_backdrop();
        /* Re-blur the capture we already have.  Without this the slider drops
         * the cache on every step, the worker grabs the screen again (8-10 ms
         * each) and the card flashes back to the wallpaper while dragging. */
        glass_live_reblur(g_cfg.blur_radius);
        render_invalidate();
        refresh();
        g_cfg_dirty = TRUE;
        break;

    default:
        break;
    }
}

static void show_menu(void)
{
    POINT pt;

    GetCursorPos(&pt);

    g_menu_host.cfg = &g_cfg;
    g_menu_host.count = (int)g_count;
    g_menu_host.size = g_size;
    g_menu_host.dark = is_dark();
    g_menu_host.scale = render_scale();

    menu_show(pt.x, pt.y);
}

/* ------------------------------------------------------------- drop hooks */

static void on_drag_enter(void)
{
    if (!g_drag_hover)
    {
        g_drag_hover = TRUE;
        refresh();
    }
}

static void on_drag_leave(void)
{
    if (g_drag_hover)
    {
        g_drag_hover = FALSE;
        refresh();
    }
}

static void on_drag_files(wchar_t **paths, int n)
{
    g_drag_hover = FALSE;
    start_recycle(paths, n);
    refresh();
}

/* ------------------------------------------------------------ hit testing */
static BOOL pt_in(const RECT *r, int x, int y)
{
    return x >= r->left && x <= r->right && y >= r->top && y <= r->bottom;
}

static int hit_pill(int x, int y)
{
    if (pt_in(&g_hit_open, x, y))
        return 1;
    if (pt_in(&g_hit_empty, x, y))
        return 2;
    return 0;
}

static BOOL pt_in_card(int x, int y)
{
    int pad = render_pad();
    return x >= pad && x < pad + render_card_w() &&
           y >= pad && y < pad + render_card_h();
}

/* --------------------------------------------------------------- window ops */

static void snap_into_screen(void)
{
    RECT r;
    int x, y, w, h;

    if (!GetWindowRect(g_hwnd, &r))
        return;

    x = r.left;
    y = r.top;
    w = r.right - r.left;
    h = r.bottom - r.top;
    clamp_position(&x, &y, w, h);

    if (x != r.left || y != r.top)
        SetWindowPos(g_hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void resize_to_scale(void)
{
    RECT r;
    int cx, cy, w, h;

    if (!GetWindowRect(g_hwnd, &r))
        return;

    cx = r.left + (r.right - r.left) / 2;
    cy = r.top + (r.bottom - r.top) / 2;
    w = render_win_w();
    h = render_win_h();

    SetWindowPos(g_hwnd, NULL, cx - w / 2, cy - h / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    snap_into_screen();
    invalidate_backdrop();
    refresh();
}

/* --------------------------------------------------------------- messages */

static void on_bin_result(void)
{
    long long c, s;

    EnterCriticalSection(&g_lock);
    c = g_q_count;
    s = g_q_size;
    LeaveCriticalSection(&g_lock);

    if (c != g_count || s != g_size)
    {
        g_count = c;
        g_size = s;
        g_cfg.last_count = c;
        g_cfg.last_size = s;
        /* The menu quotes these; it used to keep whatever it had at startup. */
        g_menu_host.count = (int)c;
        g_menu_host.size = s;
        menu_data_changed();
        update_tray_tip();
        refresh();
    }
}

static void on_op_done(void)
{
    BOOL ok;
    wchar_t msg[256];

    EnterCriticalSection(&g_lock);
    ok = g_op_ok;
    wcsncpy_s(msg, RG_COUNT(msg), g_op_msg, _TRUNCATE);
    LeaveCriticalSection(&g_lock);

    g_busy = FALSE;
    KillTimer(g_hwnd, RG_TIMER_BUSY);

    if (msg[0])
        show_toast(msg);

    if (ok)
        start_fx();

    request_query();
    refresh();
    save_cfg();
}

static void on_drop(HDROP drop)
{
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    wchar_t **paths;
    UINT i;

    if (n == 0)
    {
        DragFinish(drop);
        return;
    }

    paths = (wchar_t **)calloc(n, sizeof(wchar_t *));
    if (paths)
    {
        for (i = 0; i < n; i++)
        {
            UINT len = DragQueryFileW(drop, i, NULL, 0) + 1;
            paths[i] = (wchar_t *)calloc(len, sizeof(wchar_t));
            if (paths[i])
                DragQueryFileW(drop, i, paths[i], len);
        }

        start_recycle(paths, (int)n);

        for (i = 0; i < n; i++)
            free(paths[i]);
        free(paths);
    }

    DragFinish(drop);
    g_drag_hover = FALSE;
    refresh();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int btn;

        if (!g_tracking)
        {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_tracking = TRUE;
        }

        if (g_btn_down && !g_dragging)
        {
            if (abs(x - g_mouse_down.x) > DRAG_THRESHOLD ||
                abs(y - g_mouse_down.y) > DRAG_THRESHOLD)
            {
                /* The anchors were taken on WM_LBUTTONDOWN; every move is just
                 * (window + cursor delta). */
                g_dragging = TRUE;
                g_hover = FALSE;
                g_hover_btn = 0;
                /* Drop the hover animation outright: letting it tick would paint
                 * twice per frame for the first 160 ms of the drag. */
                g_hover_t = 0.0f;
                g_hover_target = 0;
                g_hover_ticking = FALSE;
                KillTimer(hwnd, RG_TIMER_HOVER);

                g_drag_last_x = g_drag_last_y = INT_MIN;
                g_last_frame_us = 0;
                drag_frame();
            }
        }

        if (g_dragging)
        {
            /* Drain the queued moves so a 1 kHz mouse cannot build a backlog,
             * then paint at most one frame per display refresh. */
            MSG queued;
            while (PeekMessageW(&queued, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE))
                ;
            drag_frame();
            return 0;
        }

        {
            BOOL inside = pt_in_card(x, y);
            if (inside != g_hover)
            {
                g_hover = inside;
                start_hover_anim(inside ? 1 : 0);
            }

            btn = inside ? hit_pill(x, y) : 0;
            if (btn != g_hover_btn)
            {
                g_hover_btn = btn;
                kick_hover();
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (g_hover_btn)
        {
            g_hover_btn = 0;
            kick_hover();
        }
        if (g_hover)
        {
            g_hover = FALSE;
            start_hover_anim(0);
        }
        else
        {
            refresh();
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        RECT wr;

        SetCapture(hwnd);
        g_btn_down = TRUE;
        g_mouse_down.x = GET_X_LPARAM(lp);
        g_mouse_down.y = GET_Y_LPARAM(lp);

        /* Anchor the drag here, at press time.  Doing it on the first
         * WM_MOUSEMOVE loses whatever distance the pointer covered between the
         * click and that message, and the card then trails the cursor by that
         * much for the whole drag. */
        GetCursorPos(&g_drag_cursor0);
        if (GetWindowRect(hwnd, &wr))
        {
            g_drag_win0.x = wr.left;
            g_drag_win0.y = wr.top;
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int btn;

        ReleaseCapture();
        g_btn_down = FALSE;

        if (g_dragging)
        {
            POINT cur;
            g_dragging = FALSE;
            if (g_pf.n || g_pf.iv_n)
            {
                prof_flush();
                memset(&g_pf, 0, sizeof(g_pf));
            }
            /* Land exactly where the pointer is, even if the last move was
             * rate limited away. */
            GetCursorPos(&cur);
            refresh_at(g_drag_win0.x + (cur.x - g_drag_cursor0.x),
                       g_drag_win0.y + (cur.y - g_drag_cursor0.y));
            snap_into_screen();
            invalidate_backdrop();
            save_cfg();
            refresh();
            return 0;
        }

        btn = pt_in_card(x, y) ? hit_pill(x, y) : 0;
        if (btn == 1)
            handle_command(RG_CMD_OPEN);
        else if (btn == 2)
            handle_command(RG_CMD_EMPTY);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        if (pt_in_card(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) &&
            !hit_pill(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
            handle_command(RG_CMD_OPEN);
        return 0;

    case WM_RBUTTONUP:
        show_menu();
        return 0;

    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;

    case WM_TIMER:
        switch (wp)
        {
        case RG_TIMER_POLL:
            request_query();
            /* Debounced config flush: slider drags mark the config dirty many
             * times per second, so persist at most once per poll tick. */
            if (g_cfg_dirty)
            {
                g_cfg_dirty = FALSE;
                save_cfg();
            }
            break;
        case RG_TIMER_HOVER:
            tick_hover();
            break;
        case RG_TIMER_FX:
            tick_fx();
            break;
        case RG_TIMER_TOAST:
            if (now_ms() >= g_toast_until)
            {
                g_toast[0] = 0;
                KillTimer(hwnd, RG_TIMER_TOAST);
                refresh();
            }
            break;
        case RG_TIMER_GLASS:
            if (!g_dragging)
            {
                invalidate_backdrop();
                refresh();
            }
            break;

        case RG_TIMER_BUSY:
            refresh();
            break;






        default:
            break;
        }
        return 0;

    case RG_WM_TRAY:
        switch (LOWORD(lp))
        {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            handle_command(RG_CMD_OPEN);
            break;
        case WM_RBUTTONUP:
            show_menu();
            break;
        default:
            break;
        }
        return 0;

    case RG_WM_BIN:
        on_bin_result();
        return 0;

    case RG_WM_OP:
        on_op_done();
        return 0;

    case RG_WM_PULSE:
        start_fx();
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (msg == WM_DPICHANGED && g_cfg.auto_dpi)
        {
            apply_appearance();
            resize_to_scale();
        }
        else
        {
            snap_into_screen();
            invalidate_backdrop();
            refresh();
        }
        return 0;

    case RG_WM_REFRESH:
        refresh();
        return 0;

    case RG_WM_DUMP:
        dump_frame();
        return 0;

    case WM_SETTINGCHANGE:
        invalidate_backdrop();
        refresh();
        return 0;

    case WM_CLOSE:
        diag(L"WM_CLOSE received");
        g_running = FALSE;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        diag(L"WM_DESTROY");
        KillTimer(hwnd, RG_TIMER_POLL);
        KillTimer(hwnd, RG_TIMER_HOVER);
        KillTimer(hwnd, RG_TIMER_FX);
        KillTimer(hwnd, RG_TIMER_TOAST);
        KillTimer(hwnd, RG_TIMER_BUSY);
        KillTimer(hwnd, RG_TIMER_GLASS);
        droptarget_revoke();
        remove_tray();
        PostQuitMessage(0);
        return 0;

    default:
        if (g_taskbar_msg && msg == g_taskbar_msg)
        {
            /* Explorer restarted: put the tray icon back. */
            init_tray();
            return 0;
        }
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------- setup */

static BOOL create_window(void)
{
    WNDCLASSEXW wc;
    int w = render_win_w();
    int h = render_win_h();
    int x, y;
    RECT wa;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = RG_CLASS;

    if (!RegisterClassExW(&wc))
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return FALSE;
    }

    if (g_cfg.x < 0 || g_cfg.y < 0)
    {
        work_area_at(0, 0, &wa);
        x = wa.right - w - 28;
        y = wa.bottom - h - 28;
    }
    else
    {
        x = g_cfg.x;
        y = g_cfg.y;
    }
    clamp_position(&x, &y, w, h);

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                             WS_EX_NOACTIVATE,
                             RG_CLASS, RG_APP_NAME, WS_POPUP,
                             x, y, w, h, NULL, NULL, g_inst, NULL);
    diag(L"CreateWindowExW hwnd=%p err=%lu pos=(%d,%d) size=(%dx%d)",
         (void *)g_hwnd, GetLastError(), x, y, w, h);
    if (!g_hwnd)
        return FALSE;

    DragAcceptFiles(g_hwnd, TRUE);
    return TRUE;
}

static void pulse_existing(void)
{
    HWND other = FindWindowW(RG_CLASS, NULL);
    if (other)
        PostMessageW(other, RG_WM_PULSE, 0, 0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    MSG msg;
    HANDLE mutex;

    (void)prev;
    (void)cmdline;
    (void)show;

    g_inst = inst;
    InitializeCriticalSection(&g_lock);


    /* Per-monitor DPI awareness so layout math uses real pixels. */
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetDpiCtx)(HANDLE);
        SetDpiCtx fn = (SetDpiCtx)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        if (fn)
            fn((HANDLE)-4); /* PER_MONITOR_AWARE_V2 */
    }

    mutex = CreateMutexW(NULL, TRUE, RG_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        pulse_existing();
        CloseHandle(mutex);
        return 0;
    }

    config_load(&g_cfg);
    i18n_set_language(g_cfg.language);
    g_count = g_cfg.last_count;
    g_size = g_cfg.last_size;

    diag(L"start v%ls theme=%ls size=%ls dpi=%d",
         RG_VERSION, g_cfg.theme, g_cfg.size, g_cfg.auto_dpi);

    render_init(inst);
    render_set_scale(compute_scale());
    diag(L"scale=%.3f window=%dx%d", render_scale(), render_win_w(), render_win_h());

    if (!create_window())
    {
        diag(L"create_window failed");
        return 1;
    }

    /* The DPI is only known once the window exists, so the scale is applied a
     * second time here — that is what makes "match display scaling" work. */
    {
        float s = compute_scale();
        if (fabsf(s - render_scale()) > 0.001f)
        {
            render_set_scale(s);
            resize_to_scale();
            diag(L"post-create scale=%.3f window=%dx%d", s, render_win_w(), render_win_h());
        }
    }

    g_taskbar_msg = RegisterWindowMessageW(RG_TASKBAR_MSG);
    (void)g_taskbar_msg;

    OleInitialize(NULL);
    diag(L"drop target=%d", droptarget_register(g_hwnd, on_drag_enter,
                                                on_drag_leave, on_drag_files));

    memset(&g_menu_host, 0, sizeof(g_menu_host));
    g_menu_host.cfg = &g_cfg;
    g_menu_host.on_command = menu_command;
    g_menu_host.on_set = menu_set;
    g_menu_host.on_preview = menu_preview;
    g_menu_host.query_dark = menu_query_dark;
    g_menu_host.query_scale = menu_query_scale;
    g_menu_host.scale = render_scale();
    menu_init(inst, &g_menu_host);
    apply_zorder();

    init_tray();

    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_wake && g_stop_evt)
        g_poll_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);

    detect_refresh();
    SetTimer(g_hwnd, RG_TIMER_POLL, POLL_MS, NULL);
    /* Keeps the frosted glass showing what is behind it right now, instead of
     * freezing on whatever was there when the card last repainted. */
    SetTimer(g_hwnd, RG_TIMER_GLASS, 700, NULL);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    init_capture();
    if (g_live_backdrop)
        glass_live_set_target(g_hwnd);
    refresh();
    request_query();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    diag(L"message loop ended, last wParam=%d", (int)msg.wParam);

    if (g_stop_evt)
        SetEvent(g_stop_evt);
    if (g_poll_thread)
    {
        WaitForSingleObject(g_poll_thread, 400);
        CloseHandle(g_poll_thread);
    }
    if (g_wake)
        CloseHandle(g_wake);
    if (g_stop_evt)
        CloseHandle(g_stop_evt);

    save_cfg();
    menu_shutdown();
    render_surface_free(&g_surf);
    glass_release(&g_backdrop);
    render_shutdown();

    /* Deliberately skip OleUninitialize()/CRT teardown: if the worker is still
     * inside a shell call, draining the apartment can block for seconds and
     * leaves the process alive with no window.  Everything that matters has
     * been written already, so just go. */
    ExitProcess(0);
    return 0;
}
