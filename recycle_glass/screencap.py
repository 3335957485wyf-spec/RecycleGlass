"""Capture what is actually behind a window (live "blur what's under me").

``BitBlt`` from the screen DC returns the *composited* desktop, so other
application windows are included — which is exactly what a flyout needs to
look like acrylic over a document instead of blurred wallpaper.

The caller must keep its own window out of the capture: either capture before
the window is shown, or set ``WDA_EXCLUDEFROMCAPTURE`` on it (see
``exclude_from_capture``).
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

from PIL import Image, ImageFilter

_user32 = ctypes.WinDLL("user32")
_gdi32 = ctypes.WinDLL("gdi32")

_user32.GetDC.restype = ctypes.c_void_p
_user32.GetDC.argtypes = [ctypes.c_void_p]
_user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
_gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
_gdi32.CreateCompatibleBitmap.restype = ctypes.c_void_p
_gdi32.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                          ctypes.c_int]
_gdi32.SelectObject.restype = ctypes.c_void_p
_gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
_gdi32.DeleteDC.argtypes = [ctypes.c_void_p]
_gdi32.BitBlt.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                          ctypes.c_int, ctypes.c_int, wintypes.DWORD]
_gdi32.BitBlt.restype = wintypes.BOOL
_gdi32.CreateDIBSection.restype = ctypes.c_void_p
_gdi32.CreateDIBSection.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    wintypes.UINT, ctypes.POINTER(ctypes.c_void_p),
                                    ctypes.c_void_p, wintypes.DWORD]

SRCCOPY = 0x00CC0020


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


_surface = {"dc": 0, "bmp": 0, "bits": None, "w": 0, "h": 0}


def _surface_for(w: int, h: int):
    """DIB section we can read straight from memory — GetDIBits cost 5 ms."""
    if _surface["dc"] and _surface["w"] == w and _surface["h"] == h:
        return _surface["dc"]

    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h            # top-down: row 0 is the top of the screen
    bmi.biPlanes = 1
    bmi.biBitCount = 32

    bits = ctypes.c_void_p()
    screen = _user32.GetDC(None)
    dc = _gdi32.CreateCompatibleDC(screen)
    bmp = _gdi32.CreateDIBSection(screen, ctypes.byref(bmi), 0,
                                  ctypes.byref(bits), None, 0)
    _user32.ReleaseDC(None, screen)
    if not bmp:
        return 0
    _gdi32.SelectObject(dc, bmp)

    old_bmp, old_dc = _surface["bmp"], _surface["dc"]
    if old_bmp:
        _gdi32.DeleteObject(old_bmp)
    if old_dc:
        _gdi32.DeleteDC(old_dc)
    _surface.update(dc=dc, bmp=bmp, bits=bits, w=w, h=h)
    return dc


def grab(rect: tuple[int, int, int, int]) -> Image.Image | None:
    """Return an RGB image of the screen area `rect`, or None on failure."""
    l, t, r, b = rect
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        return None
    try:
        dc = _surface_for(w, h)
        screen = _user32.GetDC(None)
        try:
            if not _gdi32.BitBlt(dc, 0, 0, w, h, screen, l, t, SRCCOPY):
                return None
        finally:
            _user32.ReleaseDC(None, screen)

        raw = ctypes.string_at(_surface["bits"], w * h * 4)
        return Image.frombuffer("RGB", (w, h), raw, "raw", "BGRX", 0, 1)
    except Exception:
        return None


def exclude_from_capture(hwnd: int) -> bool:
    """Hide this window from screen capture (Win10 2004+).

    Without it, a flyout that captures the screen would photograph itself and
    feed its own pixels back into the backdrop.
    """
    WDA_EXCLUDEFROMCAPTURE = 0x00000011
    try:
        fn = _user32.SetWindowDisplayAffinity
        fn.restype = wintypes.BOOL
        fn.argtypes = [wintypes.HWND, wintypes.DWORD]
        return bool(fn(hwnd, WDA_EXCLUDEFROMCAPTURE))
    except Exception:
        return False


def live_backdrop(rect: tuple[int, int, int, int], radius: float = 24.0):
    """Capture `rect` from the screen and return it blurred, or None.

    Blurred at half resolution — invisible after a 20 px Gaussian and ~4x
    cheaper than filtering at full size.
    """
    img = grab(rect)
    if img is None:
        return None
    # an all-black frame means the capture told us nothing (locked / secure
    # desktop) — let the caller fall back to the wallpaper instead
    try:
        hi = max(img.getextrema(), key=lambda t: t[1])[1]
        lo = min(img.getextrema(), key=lambda t: t[0])[0]
        if hi <= 4 and lo <= 4:
            return None
    except Exception:
        pass

    w, h = img.size
    factor = 2
    small = img.resize((max(1, w // factor), max(1, h // factor)),
                       Image.Resampling.BILINEAR)
    if radius > 0:
        small = small.filter(ImageFilter.GaussianBlur(max(0.5, radius / factor)))
    return small.resize((w, h), Image.Resampling.BILINEAR)
