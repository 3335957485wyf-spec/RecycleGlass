"""Monitor geometry helpers (multi-screen, mixed DPI, taskbar aware).

All coordinates here are *virtual-screen physical pixels*, the same space the
widget window lives in (the process is per-monitor-v2 DPI aware).
"""
from __future__ import annotations

import ctypes

from . import w32
from .w32 import user32, POINT, RECT, MONITORINFO


def _work_from_handle(hmon) -> tuple[int, int, int, int] | None:
    if not hmon:
        return None
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    if not user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
        return None
    r = mi.rcWork
    return (int(r.left), int(r.top), int(r.right), int(r.bottom))


def monitor_rect_at(x: int, y: int) -> tuple[int, int, int, int]:
    """Full monitor rectangle (taskbar included) containing (x, y)."""
    hmon = user32.MonitorFromPoint(POINT(int(x), int(y)),
                                   w32.MONITOR_DEFAULTTONEAREST)
    if hmon:
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            r = mi.rcMonitor
            return (int(r.left), int(r.top), int(r.right), int(r.bottom))
    return (0, 0,
            user32.GetSystemMetrics(w32.SM_CXSCREEN),
            user32.GetSystemMetrics(w32.SM_CYSCREEN))


def work_area_at(x: int, y: int) -> tuple[int, int, int, int]:
    """Work area (taskbar excluded) of the monitor containing (x, y)."""
    hmon = user32.MonitorFromPoint(POINT(int(x), int(y)),
                                   w32.MONITOR_DEFAULTTONEAREST)
    got = _work_from_handle(hmon)
    if got:
        return got
    return (0, 0,
            user32.GetSystemMetrics(w32.SM_CXSCREEN),
            user32.GetSystemMetrics(w32.SM_CYSCREEN))


def primary_work_area() -> tuple[int, int, int, int]:
    return work_area_at(0, 0)


def clamp_position(x: int, y: int, w: int, h: int,
                   margin: int = 0) -> tuple[int, int]:
    """Keep the window fully inside the work area nearest its centre.

    A saved position from a monitor that is no longer attached used to leave
    the widget invisible off-screen; this pulls it back to a visible spot.
    """
    cx, cy = int(x + w / 2), int(y + h / 2)
    l, t, r, b = work_area_at(cx, cy)
    aw, ah = max(1, r - l), max(1, b - t)
    nx = min(max(int(x), l + margin), r - w - margin)
    ny = min(max(int(y), t + margin), b - h - margin)
    if nx < l + margin:          # window wider than the work area
        nx = l
    if ny < t + margin:
        ny = t
    if w > aw or h > ah:
        nx, ny = l, t
    return nx, ny


def default_position(w: int, h: int, margin: int = 32) -> tuple[int, int]:
    """Bottom-right corner of the primary monitor's work area."""
    l, t, r, b = primary_work_area()
    return clamp_position(r - w - margin, b - h - margin, w, h, margin=0)


def dpi_for_window(hwnd: int) -> int:
    try:
        if hwnd:
            v = int(user32.GetDpiForWindow(hwnd))
            if v >= 72:
                return v
    except Exception:
        pass
    try:
        v = int(user32.GetDpiForSystem())
        if v >= 72:
            return v
    except Exception:
        pass
    return 96


def dpi_at(x: int, y: int) -> int:
    """Effective DPI of the monitor containing (x, y).

    GetDpiForMonitor lives in shcore and is missing on very old builds, so we
    fall back to the system DPI rather than guessing.
    """
    try:
        hmon = user32.MonitorFromPoint(POINT(int(x), int(y)),
                                       w32.MONITOR_DEFAULTTONEAREST)
        shcore = ctypes.WinDLL("shcore")
        dx = ctypes.c_uint()
        dy = ctypes.c_uint()
        if shcore.GetDpiForMonitor(hmon, 0, ctypes.byref(dx),
                                   ctypes.byref(dy)) == 0 and dx.value >= 72:
            return int(dx.value)
    except Exception:
        pass
    try:
        v = int(user32.GetDpiForSystem())
        if v >= 72:
            return v
    except Exception:
        pass
    return 96
