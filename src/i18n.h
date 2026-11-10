/* User-facing strings. Windows UI language picks the default; the config can
 * pin "zh" or "en". Templates use printf-style placeholders (callers format
 * them with swprintf), which keeps this table allocation-free. */
#ifndef RG_I18N_H
#define RG_I18N_H

#include "common.h"

typedef enum {
    S_APP_NAME = 0,
    S_SUB_EMPTY,
    S_SUB_COUNT,            /* %lld items · %ls */
    S_BTN_OPEN,
    S_BTN_EMPTY,
    S_DROP_HINT,
    S_TOAST_RECYCLED,       /* %lld */
    S_TOAST_NOTHING,
    S_TOAST_EMPTY_DONE,
    S_TOAST_EMPTYING,
    S_BUSY_EMPTY,           /* %lld seconds */
    S_BUSY_EMPTY_PROGRESS,  /* %lld items left */
    S_BUSY_RECYCLE,         /* %lld seconds */
    S_TOAST_RECYCLING,      /* %lld */
    S_TOAST_ALREADY_EMPTY,
    S_TOAST_FAILED,
    S_TOAST_PARTIAL,        /* %lld left */
    S_TRAY_EMPTY,
    S_TRAY_COUNT,           /* %lld items · %ls */
    S_MENU_OPEN,
    S_MENU_EMPTY,
    S_MENU_APPEARANCE,
    S_MENU_AUTOSTART,
    S_MENU_DPI,
    S_MENU_TOPMOST,
    S_MENU_EXIT,
    S_CONFIRM_TITLE,
    S_CONFIRM_HINT,         /* %lld items · %ls · permanent */
    S_CONFIRM_CANCEL,
    S_SET_OPACITY,
    S_SET_MATERIAL,
    S_SET_BLUR,
    S_SET_THEME,
    S_SET_SIZE,
    S_SET_BLUR_MODE,
    S_OPT_SYSTEM,
    S_OPT_LIGHT,
    S_OPT_DARK,
    S_OPT_SMALL,
    S_OPT_NORMAL,
    S_OPT_LARGE,
    S_OPT_NATIVE,
    S_OPT_WALLPAPER,
    S_STATUS_EMPTY,
    S_SETTINGS_TITLE,
    S_MENU_RESET,
    S_COUNT
} RgStr;

void          i18n_set_language(const wchar_t *pref); /* "auto" | "zh" | "en" */
const wchar_t *i18n_language(void);
BOOL          i18n_is_cjk(void);
const wchar_t *T(RgStr id);

#endif /* RG_I18N_H */
