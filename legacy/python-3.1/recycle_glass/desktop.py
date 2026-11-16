"""Pin a window to the desktop layer (survives hiding desktop icons).

Two desktop layouts exist on Windows 11:

* Classic: Progman owns SHELLDLL_DefView (icons); after a 0x052C poke the shell
  creates a top-level WorkerW that hosts the wallpaper.
* Raised desktop (Win11 22H2+/24H2+, the current machine's build): Progman
  carries WS_EX_NOREDIRECTIONBITMAP and the WorkerW is a child of Progman.
  Lively Wallpaper and friends reparent to Progman instead.

Either way the widget ends up as a child of the desktop hierarchy, so normal
top-level windows cover it, the taskbar/Alt-Tab ignore it, and toggling
"Show desktop icons" does not remove it.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

from . import w32
from .w32 import (
    ENUMPROC,
    POINT,
    SIZE,
    BLENDFUNCTION,
    user32,
    kernel32,
    gdi32,
)


def _find_workerw_with_defview() -> tuple[int | None, int | None]:
    """Return (host_window, defview). host_window is the window that owns DefView."""
    result: dict[str, int | None] = {"host": None, "defview": None}

    @ENUMPROC
    def cb(hwnd, _lparam):
        defview = user32.FindWindowExW(hwnd, None, "SHELLDLL_DefView", None)
        if defview:
            result["host"] = hwnd
            result["defview"] = defview
            return False
        return True

    user32.EnumWindows(cb, 0)
    return result["host"], result["defview"]


def _find_workerw_wallpaper() -> int | None:
    """Find the wallpaper WorkerW (top-level, no DefView child)."""
    found: dict[str, int | None] = {"w": None}

    @ENUMPROC
    def cb(hwnd, _lparam):
        if user32.FindWindowExW(hwnd, None, "SHELLDLL_DefView", None):
            # next sibling WorkerW is the wallpaper host in classic layout
            nxt = user32.FindWindowExW(None, hwnd, "WorkerW", None)
            if nxt:
                found["w"] = nxt
                return False
        return True

    user32.EnumWindows(cb, 0)
    return found["w"]


def find_desktop_workerw() -> int:
    """Lively route: ask Progman to spawn the wallpaper WorkerW, then find it.

    SendMessageTimeout(0x052C) tells Progman to create a WorkerW *behind* the
    desktop icons.  The wallpaper host is then the top-level window that owns
    a SHELLDLL_DefView child — its next WorkerW sibling is what we want.
    """
    progman = user32.FindWindowW("Progman", None)
    if not progman:
        return 0
    result = ctypes.c_void_p()
    try:
        user32.SendMessageTimeoutW(progman, 0x052C, 0, 0, 0x0002, 1000,
                                   ctypes.byref(result))
    except Exception:
        pass

    found = {"w": 0}

    @w32.ENUMPROC
    def cb(tophandle, lp):
        view = user32.FindWindowExW(tophandle, 0, "SHELLDLL_DefView", None)
        if view:
            sib = user32.FindWindowExW(0, tophandle, "WorkerW", None)
            if sib:
                found["w"] = int(sib)
        return True

    user32.EnumWindows(cb, 0)

    # The shell spawns several WorkerW windows; the one we want is the
    # full-screen wallpaper host.  Picking "the next WorkerW sibling" can land
    # on a small hidden one, so rank by area and take the largest.
    best = 0
    best_area = 0

    @w32.ENUMPROC
    def pick(top, lp):
        nonlocal best, best_area
        b = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(top, b, 256)
        if b.value == "WorkerW":
            r = w32.RECT()
            user32.GetWindowRect(top, ctypes.byref(r))
            area = max(0, r.right - r.left) * max(0, r.bottom - r.top)
            if area > best_area:
                best_area = area
                best = int(top)
        return True

    user32.EnumWindows(pick, 0)
    return best or found["w"] or int(progman)


def attach_to_desktop(hwnd: int) -> str:
    """Embed the widget in the desktop layer (Lively Wallpaper's route).

    SetParent() makes the widget a *child* of the wallpaper WorkerW, which is
    exactly where a desktop gadget belongs: above the wallpaper, below every
    normal window.

    A top-level window cannot express that position — passing the desktop as
    hWndInsertAfter drops us *behind* the wallpaper, and HWND_TOPMOST floats
    us over applications.  Making it a child sidesteps both.

    Requires a plain (non-layered) window: UpdateLayeredWindow silently fails
    for children of the raised desktop, so content is drawn with AlphaBlend.
    """
    # Lively's SetParent-to-WorkerW route was tried and rejected on this
    # shell: the widget ended up behind the desktop icon list (WindowFromPoint
    # returned SysListView32) with IsWindowVisible False.  A top-level window
    # is the only position that is reliably visible here.
    return "toplevel"


def find_desktop_anchor() -> int:
    """Return the Z-order anchor we should sit *above*.

    On Win11 raised desktop the wallpaper lives inside Progman, so inserting
    after Progman places us between the wallpaper and every normal window.
    On classic layouts the wallpaper WorkerW is a top-level sibling — use that
    instead. Falls back to Progman, then 0 (desktop).
    """
    progman = user32.FindWindowW("Progman", None)
    if progman:
        exstyle = user32.GetWindowLongPtrW(progman, w32.GWL_EXSTYLE)
        if exstyle & w32.WS_EX_NOREDIRECTIONBITMAP:
            return int(progman)
    # classic: find the wallpaper WorkerW (top-level WorkerW without DefView)
    wallpaper = _find_workerw_wallpaper()
    if wallpaper:
        return int(wallpaper)
    if progman:
        return int(progman)
    return 0


def reassert_zorder(hwnd: int, native: bool = False) -> None:
    """Keep the widget pinned to the desktop layer (original behaviour).

    hWndInsertAfter is the window that stays ABOVE the placed one, so the
    desktop anchor puts us right behind it — for a layered window that is
    still visible because DWM composites the layer separately.
    """
    if not user32.IsWindow(hwnd):
        return
    anchor = find_desktop_anchor()
    user32.SetWindowPos(
        hwnd,
        anchor if anchor else w32.HWND_BOTTOM,
        0, 0, 0, 0,
        w32.SWP_NOMOVE | w32.SWP_NOSIZE | w32.SWP_NOACTIVATE,
    )


DESKTOP_CLASSES = ("Progman", "WorkerW")


def _occluded(hwnd: int) -> bool:
    """True when the DESKTOP covers the widget, not an application.

    Being covered by another app is normal for a desktop gadget — the user
    clicks that window and the widget should stay behind it.  Only the
    shell swallowing us is a defect worth fighting.
    """
    try:
        r = w32.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        pt = POINT((r.left + r.right) // 2, (r.top + r.bottom) // 2)
        hit = user32.WindowFromPoint(pt)
        if hit == hwnd:
            return False
        if not hit:
            return True
        buf = wintypes.WCHAR * 256
        b = buf()
        user32.GetClassNameW(hit, b, 256)
        return b.value in DESKTOP_CLASSES
    except Exception:
        return False


_layered_cache: dict[tuple[int, int], dict] = {}


def _get_layered_surface(width: int, height: int):
    """Get (or create) a cached screen DC + memory DC + DIB section."""
    key = (width, height)
    ent = _layered_cache.get(key)
    if ent is not None:
        return ent

    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)

    bmi = w32.BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(w32.BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = width
    bmi.bmiHeader.biHeight = -height  # top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = 0  # BI_RGB

    bits = ctypes.c_void_p()
    bmp = gdi32.CreateDIBSection(
        hdc_mem, ctypes.byref(bmi), w32.DIB_RGB_COLORS, ctypes.byref(bits), None, 0
    )
    if not bmp or not bits.value:
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(0, hdc_screen)
        return None

    old = gdi32.SelectObject(hdc_mem, bmp)
    ent = {"screen": hdc_screen, "dc": hdc_mem, "bmp": bmp,
           "bits": bits, "old": old, "buf": None}
    # evict the oldest surface — DC + DIB are GDI handles and leaking them
    # makes the UI hang after a few size changes
    while len(_layered_cache) >= 6:
        old_key, old_ent = next(iter(_layered_cache.items()))
        try:
            if old_ent["bmp"]:
                gdi32.DeleteObject(old_ent["bmp"])
            if old_ent["dc"]:
                gdi32.DeleteDC(old_ent["dc"])
        except Exception:
            pass
        _layered_cache.pop(old_key, None)

    _layered_cache[key] = ent
    return ent


def update_layered(
    hwnd: int,
    width: int,
    height: int,
    x: int | None,
    y: int | None,
    bgra: bytes,
    alpha: int = 255,
) -> bool:
    """Push a premultiplied-BGRA pixel buffer to a layered window.

    Pass x/y as None to leave the window position untouched (the common case
    during repaint). Supplying a position moves the window, which is only
    wanted when the user is actually dragging it.

    GDI objects are cached per (width, height) so repeated repaints don't
    allocate a fresh DC + DIB section every frame.
    """
    ent = _get_layered_surface(width, height)
    if ent is None:
        return False
    hdc_screen = ent["screen"]
    hdc_mem = ent["dc"]

    # Animation frames usually reuse the same bitmap and only change the
    # constant alpha — skip the 600 KB copy in that case.
    if ent["buf"] is not bgra:
        ctypes.memmove(ent["bits"].value, bgra, len(bgra))
        ent["buf"] = bgra

    psize = SIZE(width, height)
    ppt_src = POINT(0, 0)
    # SourceConstantAlpha lets us fade the whole surface in the compositor —
    # no per-frame pixel work, so animation frames stay cheap.
    blend = BLENDFUNCTION(w32.AC_SRC_OVER, 0, max(0, min(255, alpha)),
                          w32.AC_SRC_ALPHA)

    if x is not None and y is not None:
        ppt_dst = POINT(x, y)
        pptr = ctypes.byref(ppt_dst)
    else:
        pptr = None  # keep current window position

    ok = user32.UpdateLayeredWindow(
        hwnd,
        hdc_screen,
        pptr,
        ctypes.byref(psize),
        hdc_mem,
        ctypes.byref(ppt_src),
        0,
        ctypes.byref(blend),
        w32.ULW_ALPHA,
    )
    err = ctypes.get_last_error()

    if not ok:
        import os as _os
        import time as _time
        try:
            lp = _os.path.join(_os.environ.get("TEMP", "."), "recycle_glass.log")
            with open(lp, "a", encoding="utf-8") as f:
                f.write(f"[{_time.strftime('%H:%M:%S')}] ULW FAIL hwnd={hwnd} err={err} {width}x{height}\n")
        except OSError:
            pass
    return bool(ok)


def icon_to_bgra(hicon: int) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    """Convert an HICON to (w, h, pixels) with (r,g,b,a) tuples."""
    info = w32.ICONINFO()
    if not user32.GetIconInfo(hicon, ctypes.byref(info)):
        return 0, 0, []

    try:
        bmp = info.hbmColor if info.hbmColor else info.hbmMask
        if not bmp:
            return 0, 0, []

        # Read the BITMAP header to get dimensions.
        class BITMAP(ctypes.Structure):
            _fields_ = [
                ("bmType", wintypes.LONG),
                ("bmWidth", wintypes.LONG),
                ("bmHeight", wintypes.LONG),
                ("bmWidthBytes", wintypes.LONG),
                ("bmPlanes", wintypes.WORD),
                ("bmBitsPixel", wintypes.WORD),
                ("bmBits", ctypes.c_void_p),
            ]

        bm = BITMAP()
        gdi32.GetObjectW.argtypes = [wintypes.HGDIOBJ, ctypes.c_int, ctypes.c_void_p]
        gdi32.GetObjectW.restype = ctypes.c_int
        gdi32.GetObjectW(bmp, ctypes.sizeof(bm), ctypes.byref(bm))

        w, h = bm.bmWidth, abs(bm.bmHeight)
        if w <= 0 or h <= 0:
            return 0, 0, []

        bmi = w32.BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(w32.BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = w
        bmi.bmiHeader.biHeight = -h
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = 0

        buf = (ctypes.c_ubyte * (w * h * 4))()
        hdc = user32.GetDC(0)
        gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bmi), w32.DIB_RGB_COLORS)
        user32.ReleaseDC(0, hdc)

        pixels: list[tuple[int, int, int, int]] = []
        for i in range(w * h):
            b = buf[i * 4 + 0]
            g = buf[i * 4 + 1]
            r = buf[i * 4 + 2]
            a = buf[i * 4 + 3]
            pixels.append((r, g, b, a))
        return w, h, pixels
    finally:
        if info.hbmColor:
            gdi32.DeleteObject(info.hbmColor)
        if info.hbmMask:
            gdi32.DeleteObject(info.hbmMask)


def load_image_as_hicon(path: str, cx: int = 32, cy: int = 32) -> int:
    """Load a file as an HICON (LR_LOADFROMFILE | LR_DEFAULTSIZE fallback)."""
    IMAGE_ICON = 1
    LR_LOADFROMFILE = 0x0010
    LR_DEFAULTSIZE = 0x0040
    handle = user32.LoadImageW(None, path, IMAGE_ICON, cx, cy, LR_LOADFROMFILE)
    return int(handle or 0)


# ---------------------------------------------------------------------------
# Native (non-layered) presentation: DWM provides the blur, we AlphaBlend
# the content on top.  UpdateLayeredWindow and the DWM backdrop share one
# composition slot, so a window can use one or the other, never both.
# ---------------------------------------------------------------------------

msimg32 = ctypes.WinDLL("msimg32")
# BLENDFUNCTION is a 4-byte POD passed BY VALUE on x64; declaring the
# parameter as a ctypes Structure makes ctypes hand over a pointer instead,
# and AlphaBlend silently paints nothing.  Pack it into a DWORD.
msimg32.AlphaBlend.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, ctypes.c_int, wintypes.HDC,
                               ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, wintypes.DWORD]
msimg32.AlphaBlend.restype = wintypes.BOOL

DWMWA_WINDOW_CORNER_PREFERENCE = 33
DWMWA_SYSTEMBACKDROP_TYPE = 38
DWMWCP_ROUND = 2
DWMSBT_TRANSIENTWINDOW = 3     # acrylic


def native_blur_supported() -> bool:
    """True when DWMWA_SYSTEMBACKDROP_TYPE exists (Win11 22H2 / build 22621+)."""
    try:
        import sys
        if sys.getwindowsversion().build < 22621:
            return False
        ctypes.WinDLL("dwmapi")
        return True
    except Exception:
        return False


def enable_native_blur(hwnd) -> bool:
    """System-rounded corners + acrylic backdrop. True on Win11 22H2+."""
    try:
        dwm = ctypes.WinDLL("dwmapi")
        dwm.DwmSetWindowAttribute.argtypes = [
            wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        dwm.DwmSetWindowAttribute.restype = ctypes.c_long
        c = ctypes.c_int(DWMWCP_ROUND)
        dwm.DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                  ctypes.byref(c), ctypes.sizeof(c))
        c = ctypes.c_int(DWMSBT_TRANSIENTWINDOW)
        hr = dwm.DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                       ctypes.byref(c), ctypes.sizeof(c))
        return hr == 0
    except Exception:
        return False


def blit_bgra(dest_dc, w: int, h: int, bgra: bytes, alpha: int = 255) -> None:
    """Composite an RGBA buffer onto a window DC with per-pixel alpha."""
    ent = _get_layered_surface(w, h)
    if ent is None:
        return
    ctypes.memmove(ent["bits"].value, bgra, len(bgra))
    a = max(0, min(255, int(alpha)))
    packed = (w32.AC_SRC_ALPHA << 24) | (a << 16)  # BlendOp=SRC_OVER(0)
    msimg32.AlphaBlend(dest_dc, 0, 0, w, h, ent["dc"], 0, 0, w, h, packed)


def present(hwnd, w: int, h: int, x: int, y: int, bgra: bytes,
            alpha: int = 255, native: bool = False) -> None:
    """Show the surface: ULW for the layered mode, AlphaBlend for native."""
    if native:
        hdc = user32.GetDC(hwnd)
        if hdc:
            blit_bgra(hdc, w, h, bgra, alpha)
            user32.ReleaseDC(hwnd, hdc)
    else:
        update_layered(hwnd, w, h, x, y, bgra, alpha)
