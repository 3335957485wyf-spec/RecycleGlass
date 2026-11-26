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
#define RG_VERSION   L"4.15.10"
/* Shared squircle exponent: flatter shoulders than a rounded rectangle, with a
 * tighter, more deliberate corner than a circle. */
#define RG_SQUIRCLE_N 2.0f

/* ---- Apple-style concentric geometry ------------------------------------
 * One golden unit U = R / phi drives every spacing in the UI.  Nested levels
 * use U, U/phi, U/phi^2, U/phi^3 - never an arbitrary 10/12/8/4 px mixture.
 * A shape inset by d from its parent takes radius (R_parent - d), so both
 * corner arcs share the same centre: this is Apple's "concentric corners".
 * With d = R/phi the inner radius becomes R/phi^2, i.e. the golden series
 * R, R/phi^2, ... - the gaps and the radii come from the same number. */
#define RG_PHI 1.6180339887498948f

static inline float rg_phi_unit(float outer_radius)
{
    return outer_radius / RG_PHI;
}

static inline int rg_phi_step(float unit, int k)
{
    float v = unit;
    int i;
    for (i = 0; i < k; i++)
        v /= RG_PHI;
    return (int)lroundf(v);
}

static inline float rg_concentric(float outer_radius, float inset)
{
    float r = outer_radius - inset;
    return r > 0.5f ? r : 0.5f;
}

#define RG_CLASS     L"RecycleGlassWidgetC"
#define RG_MUTEX     L"RecycleGlassSingletonC"
#define RG_TASKBAR_MSG L"TaskbarCreated"

/* ---- custom messages ---------------------------------------------------- */
#define RG_WM_TRAY   (WM_APP + 1)
#define RG_WM_BIN    (WM_APP + 2)
#define RG_WM_OP     (WM_APP + 3)
#define RG_WM_PULSE  (WM_APP + 4)
#define RG_WM_REFRESH (WM_APP + 7)  /* a new backdrop capture landed */
#define RG_WM_DUMP   (WM_APP + 9)   /* write the current frame to %TEMP% */
#define RG_WM_BINWATCH (WM_APP + 10) /* shell: recycle bin contents changed */
#define RG_WM_DRAG_TICK (WM_APP + 11) /* drag settle / landing sample */
#define RG_WM_CMD     (WM_APP + 12) /* handle_command(wparam) - automation hook */
#define RG_WM_DUMP_BD (WM_APP + 14)  /* write the backdrop tile + rim to %TEMP% */

/* ---- timers ------------------------------------------------------------- */
#define RG_TIMER_POLL   1
#define RG_TIMER_HOVER  2
#define RG_TIMER_FX     3
#define RG_TIMER_TOAST  4
#define RG_TIMER_BUSY   5
#define RG_TIMER_GLASS  6   /* keeps the live backdrop fresh */

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
#define RG_CMD_RESET          160
#define RG_SET_OPACITY        170   /* continuous slider values */
#define RG_SET_MATERIAL       171
#define RG_SET_BLUR           172
#define RG_SET_CORNER         173
#define RG_SET_REFRACTION     174
#define RG_SET_MOTION         175
#define RG_CMD_GLASS_CRYSTAL  180
#define RG_CMD_GLASS_AURORA   181
#define RG_CMD_GLASS_MIST     182
#define RG_CMD_ACCENT_BASE    190   /* + accent index: blue violet mint peach amber lime */
#define RG_CMD_ICON_ACCENT    200   /* app mark follows the accent */
#define RG_CMD_CAPTURE        202   /* show the widget in screenshots */
#define RG_CMD_SMART_CAPTURE  204   /* visible only while screenshot tool is foreground */
#define RG_CMD_ICON_CLASSIC   201   /* original blue chip */

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
