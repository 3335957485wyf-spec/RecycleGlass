/* RecycleGlass-C — shared definitions.
 *
 * A C rewrite of the Python "RecycleGlass" desktop widget: a Fluent-style
 * glass card that shows the Windows Recycle Bin, can open it, empty it, and
 * accepts files dropped onto it (which are sent to the recycle bin).
 */
#ifndef RG_COMMON_H
#define RG_COMMON_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#define RG_APP_NAME  L"RecycleGlass"
#define RG_VERSION   L"4.0.0"
#define RG_CLASS     L"RecycleGlassWidgetC"
#define RG_MENUCLASS L"RecycleGlassMenu"
#define RG_MUTEX     L"RecycleGlassSingletonC"
#define RG_TASKBAR_MSG L"TaskbarCreated"

/* ---- custom messages ---------------------------------------------------- */
#define RG_WM_TRAY   (WM_APP + 1)
#define RG_WM_BIN    (WM_APP + 2)
#define RG_WM_OP     (WM_APP + 3)
#define RG_WM_PULSE  (WM_APP + 4)
#define RG_WM_REFRESH (WM_APP + 7)  /* a new backdrop capture landed */
#define RG_WM_DUMP   (WM_APP + 9)   /* write the current frame to %TEMP% */

/* ---- timers ------------------------------------------------------------- */
#define RG_TIMER_POLL   1
#define RG_TIMER_HOVER  2
#define RG_TIMER_FX     3
#define RG_TIMER_TOAST  4
#define RG_TIMER_BUSY   5
#define RG_TIMER_GLASS  6   /* keeps the live backdrop fresh */
#define RG_TIMER_DRAG   7   /* one frame per display refresh while dragging */

/* ---- menu commands ------------------------------------------------------ */
#define RG_CMD_OPEN           101
#define RG_CMD_EMPTY          102
#define RG_CMD_AUTOSTART      104
#define RG_CMD_EXIT           105
#define RG_CMD_DPI            106
#define RG_CMD_TOPMOST        107
#define RG_CMD_EMPTY_NOW      108   /* empty without a second prompt */
#define RG_CMD_THEME_SYSTEM   111
#define RG_CMD_THEME_LIGHT    112
#define RG_CMD_THEME_DARK     113
#define RG_CMD_SIZE_SMALL     121
#define RG_CMD_SIZE_NORMAL    122
#define RG_CMD_SIZE_LARGE     123
#define RG_CMD_BLUR_BASE      130   /* + index into g_blur_steps */
#define RG_CMD_OPACITY_BASE   140   /* + index into g_opacity_steps */
#define RG_CMD_MATERIAL_BASE  150   /* + index into g_material_steps */
#define RG_CMD_BLUR_SCREEN    161   /* blur the real desktop (hidden in screenshots) */
#define RG_CMD_BLUR_WALLPAPER 162   /* blur the wallpaper (visible in screenshots) */
#define RG_CMD_RESET          160
#define RG_SET_OPACITY        170   /* continuous slider values */
#define RG_SET_MATERIAL       171
#define RG_SET_BLUR           172

/* ---- small helpers ------------------------------------------------------ */
static __inline float rg_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static __inline int rg_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Matches render._ease_out(): 1 - (1-u)^3 with u clamped to [0,1]. */
static __inline float rg_ease_out(float u)
{
    float k;
    u = rg_clampf(u, 0.0f, 1.0f);
    k = 1.0f - u;
    return 1.0f - k * k * k;
}

static __inline float rg_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* Number of elements in a fixed array. */
#define RG_COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

#endif /* RG_COMMON_H */
