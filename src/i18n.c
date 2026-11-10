#include "i18n.h"

static const wchar_t *g_zh[S_COUNT] = {
    /* S_APP_NAME            */ L"回收站",
    /* S_SUB_EMPTY           */ L"空",
    /* S_SUB_COUNT           */ L"%lld 项 · %ls",
    /* S_BTN_OPEN            */ L"打开",
    /* S_BTN_EMPTY           */ L"清空",
    /* S_DROP_HINT           */ L"松开以回收",
    /* S_TOAST_RECYCLED      */ L"已回收 %lld 项",
    /* S_TOAST_NOTHING       */ L"未识别到文件",
    /* S_TOAST_EMPTY_DONE    */ L"已清空",
    /* S_TOAST_EMPTYING      */ L"正在清空…",
    /* S_BUSY_EMPTY          */ L"正在清空… %llds",
    /* S_BUSY_EMPTY_PROGRESS */ L"正在清空… 剩余 %lld 项",
    /* S_BUSY_RECYCLE        */ L"正在回收… %llds",
    /* S_TOAST_RECYCLING     */ L"正在回收 %lld 项…",
    /* S_TOAST_ALREADY_EMPTY */ L"回收站已经是空的",
    /* S_TOAST_FAILED        */ L"操作失败",
    /* S_TOAST_PARTIAL       */ L"部分失败，剩余 %lld 项",
    /* S_TRAY_EMPTY          */ L"回收站 · 空",
    /* S_TRAY_COUNT          */ L"回收站 · %lld 项 · %ls",
    /* S_MENU_OPEN           */ L"打开回收站",
    /* S_MENU_EMPTY          */ L"清空回收站",
    /* S_MENU_APPEARANCE     */ L"外观设置",
    /* S_MENU_AUTOSTART      */ L"开机自启动",
    /* S_MENU_DPI            */ L"跟随系统缩放",
    /* S_MENU_TOPMOST        */ L"总在最前",
    /* S_MENU_EXIT           */ L"退出",
    /* S_CONFIRM_TITLE       */ L"清空回收站？",
    /* S_CONFIRM_HINT        */ L"%lld 项 · %ls · 无法恢复",
    /* S_CONFIRM_CANCEL      */ L"取消",
    /* S_SET_OPACITY         */ L"透明度",
    /* S_SET_MATERIAL        */ L"材质浓度",
    /* S_SET_BLUR            */ L"模糊半径",
    /* S_SET_THEME           */ L"主题",
    /* S_SET_SIZE            */ L"界面大小",
    /* S_SET_BLUR_MODE       */ L"模糊效果",
    /* S_OPT_SYSTEM          */ L"跟随",
    /* S_OPT_LIGHT           */ L"浅色",
    /* S_OPT_DARK            */ L"深色",
    /* S_OPT_SMALL           */ L"小",
    /* S_OPT_NORMAL          */ L"标准",
    /* S_OPT_LARGE           */ L"大",
    /* S_OPT_NATIVE          */ L"桌面",
    /* S_OPT_WALLPAPER       */ L"壁纸",
    /* S_STATUS_EMPTY        */ L"回收站为空",
    /* S_SETTINGS_TITLE      */ L"设置",
    /* S_MENU_RESET          */ L"恢复默认外观",
};

static const wchar_t *g_en[S_COUNT] = {
    L"Recycle Bin",
    L"Empty",
    L"%lld items · %ls",
    L"Open",
    L"Empty",
    L"Release to recycle",
    L"%lld item(s) recycled",
    L"No files recognised",
    L"Recycle bin emptied",
    L"Emptying…",
    L"Emptying… %llds",
    L"Emptying… %lld left",
    L"Recycling… %llds",
    L"Recycling %lld item(s)…",
    L"Recycle bin is already empty",
    L"Operation failed",
    L"Partly failed, %lld left",
    L"Recycle Bin · Empty",
    L"Recycle Bin · %lld items · %ls",
    L"Open Recycle Bin",
    L"Empty Recycle Bin",
    L"Appearance",
    L"Start with Windows",
    L"Match display scaling",
    L"Always on top",
    L"Exit",
    L"Empty Recycle Bin?",
    L"%lld items · %ls · permanent",
    L"Cancel",
    L"Opacity",
    L"Material",
    L"Blur radius",
    L"Theme",
    L"Size",
    L"Blur",
    L"Auto",
    L"Light",
    L"Dark",
    L"S",
    L"M",
    L"L",
    L"Native",
    L"Wallpaper",
    L"Recycle bin is empty",
    L"Settings",
    L"Reset appearance",
};

static const wchar_t **g_table = g_zh;
static wchar_t g_lang[4] = L"zh";

static const wchar_t *system_language(void)
{
    LANGID langid = GetUserDefaultUILanguage();
    /* Primary language id 4 == Chinese. */
    return ((langid & 0x3FF) == 4) ? L"zh" : L"en";
}

void i18n_set_language(const wchar_t *pref)
{
    if (!pref || (wcscmp(pref, L"zh") != 0 && wcscmp(pref, L"en") != 0))
        pref = L"auto";

    if (wcscmp(pref, L"auto") == 0)
        wcscpy_s(g_lang, RG_COUNT(g_lang), system_language());
    else
        wcscpy_s(g_lang, RG_COUNT(g_lang), pref);

    g_table = (wcscmp(g_lang, L"zh") == 0) ? g_zh : g_en;
}

const wchar_t *i18n_language(void)
{
    return g_lang;
}

BOOL i18n_is_cjk(void)
{
    return wcscmp(g_lang, L"zh") == 0;
}

const wchar_t *T(RgStr id)
{
    if ((int)id < 0 || (int)id >= S_COUNT)
        return L"";
    if (!g_table[id])
        return g_zh[id];
    return g_table[id];
}
