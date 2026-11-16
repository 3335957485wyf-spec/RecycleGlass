"""Tiny string table so the widget speaks the user's language.

Windows UI language decides the default; the config can pin ``zh`` or ``en``.
Only strings that are actually shown to the user live here — the card, the
tray tooltip, toasts and the context menu.
"""
from __future__ import annotations

import ctypes

LANGUAGES = ("auto", "zh", "en")

_ZH = {
    "app_name": "回收站",
    "sub_empty": "空",
    "sub_count": "{n} 项 · {size}",
    "btn_open": "打开",
    "btn_empty": "清空",
    "drop_hint": "松开以回收",
    "toast_recycled": "已回收 {n} 项",
    "toast_nothing": "未识别到文件",
    "toast_empty_done": "已清空",
    "toast_emptying": "正在清空…",
    "busy_empty": "正在清空… {s}s",
    "busy_empty_progress": "正在清空… 剩余 {n} 项",
    "busy_recycle": "正在回收… {s}s",
    "toast_recycling": "正在回收 {n} 项…",
    "toast_already_empty": "回收站已经是空的",
    "toast_failed": "操作失败",
    "toast_partial": "部分失败，剩余 {n} 项",
    "tray_empty": "回收站 · 空",
    "tray_count": "回收站 · {n} 项 · {size}",
    "menu_open": "打开回收站",
    "menu_empty": "清空回收站",
    "menu_appearance": "外观设置",
    "menu_autostart": "开机自启动",
    "menu_dpi": "跟随系统缩放",
    "menu_exit": "退出",
    "confirm_title": "清空回收站？",
    "confirm_hint": "{n} 项 · {size} · 无法恢复",
    "confirm_cancel": "取消",
    "set_opacity": "透明度",
    "set_material": "材质浓度",
    "set_blur": "模糊半径",
    "set_theme": "主题",
    "set_size": "界面大小",
    "set_blur_mode": "模糊效果",
    "opt_system": "跟随",
    "opt_light": "浅色",
    "opt_dark": "深色",
    "opt_small": "小",
    "opt_normal": "标准",
    "opt_large": "大",
    "opt_native": "原生",
    "opt_wallpaper": "壁纸",
    "status_empty": "回收站为空",
    "settings_title": "设置",
}

_EN = {
    "app_name": "Recycle Bin",
    "sub_empty": "Empty",
    "sub_count": "{n} items · {size}",
    "btn_open": "Open",
    "btn_empty": "Empty",
    "drop_hint": "Release to recycle",
    "toast_recycled": "{n} item(s) recycled",
    "toast_nothing": "No files recognised",
    "toast_empty_done": "Recycle bin emptied",
    "toast_emptying": "Emptying…",
    "busy_empty": "Emptying… {s}s",
    "busy_empty_progress": "Emptying… {n} left",
    "busy_recycle": "Recycling… {s}s",
    "toast_recycling": "Recycling {n} item(s)…",
    "toast_already_empty": "Recycle bin is already empty",
    "toast_failed": "Operation failed",
    "toast_partial": "Partly failed, {n} left",
    "tray_empty": "Recycle Bin · Empty",
    "tray_count": "Recycle Bin · {n} items · {size}",
    "menu_open": "Open Recycle Bin",
    "menu_empty": "Empty Recycle Bin",
    "menu_appearance": "Appearance",
    "menu_autostart": "Start with Windows",
    "menu_dpi": "Match display scaling",
    "menu_exit": "Exit",
    "confirm_title": "Empty Recycle Bin?",
    "confirm_hint": "{n} items · {size} · permanent",
    "confirm_cancel": "Cancel",
    "set_opacity": "Opacity",
    "set_material": "Material",
    "set_blur": "Blur radius",
    "set_theme": "Theme",
    "set_size": "Size",
    "set_blur_mode": "Blur",
    "opt_system": "Auto",
    "opt_light": "Light",
    "opt_dark": "Dark",
    "opt_small": "S",
    "opt_normal": "M",
    "opt_large": "L",
    "opt_native": "Native",
    "opt_wallpaper": "Wallpaper",
    "status_empty": "Recycle bin is empty",
    "settings_title": "Settings",
}

_TABLES = {"zh": _ZH, "en": _EN}
_current = "zh"


def _system_language() -> str:
    try:
        user32 = ctypes.WinDLL("user32")
        langid = user32.GetUserDefaultUILanguage() & 0xFFFF
        # primary language id 0x04 == Chinese
        return "zh" if (langid & 0x3FF) == 0x04 else "en"
    except Exception:
        return "zh"


def set_language(pref: str) -> str:
    """Resolve and store the active language. Returns 'zh' or 'en'."""
    global _current
    pref = pref if pref in LANGUAGES else "auto"
    _current = _system_language() if pref == "auto" else pref
    return _current


def language() -> str:
    return _current


def is_cjk() -> bool:
    return _current == "zh"


def t(key: str, **kw) -> str:
    table = _TABLES.get(_current, _ZH)
    s = table.get(key) or _ZH.get(key) or key
    if kw:
        try:
            return s.format(**kw)
        except (KeyError, IndexError, ValueError):
            return s
    return s
