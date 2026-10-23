"""Open the flyout and capture how it actually looks on screen.

    python tools/check_menu.py [out.png]

The panel is a topmost layered window, so a plain BitBlt of the screen shows
the real composite (menu over whatever is behind it) — unlike PrintWindow,
which flattens alpha onto white.
"""
from __future__ import annotations

import ctypes
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

from PIL import Image  # noqa: E402

from recycle_glass import w32  # noqa: E402
from recycle_glass.w32 import user32  # noqa: E402

gdi32 = ctypes.WinDLL("gdi32")
user32.GetDC.restype = ctypes.c_void_p
user32.GetDC.argtypes = [ctypes.c_void_p]
user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
gdi32.CreateCompatibleBitmap.restype = ctypes.c_void_p
gdi32.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = ctypes.c_void_p
gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.BitBlt.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                         ctypes.c_uint]
gdi32.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                            ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_uint]


class BIH(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_ushort),
                ("biBitCount", ctypes.c_ushort), ("biCompression", ctypes.c_uint),
                ("biSizeImage", ctypes.c_uint), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint),
                ("biClrImportant", ctypes.c_uint)]


def capture(x, y, w, h) -> Image.Image:
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020)
    bmi = BIH()
    bmi.biSize = ctypes.sizeof(BIH)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    user32.ReleaseDC(None, screen)
    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")


def find(cls: str) -> list[int]:
    out: list[int] = []

    @w32.ENUMPROC
    def cb(h, l):
        b = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, b, 256)
        if b.value == cls:
            out.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return out


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "shot_menu.png"
    pos = w32.POINT()
    user32.GetCursorPos(ctypes.byref(pos))
    origin = (pos.x, pos.y)

    # open the flyout over whatever is on screen at (620, 720)
    user32.SetCursorPos(620, 720)
    time.sleep(0.2)
    widgets = find("RecycleGlassWidget")
    if not widgets:
        print("[FAIL] widget not running")
        raise SystemExit(1)
    user32.PostMessageW(widgets[0], 0x0205, 0, 0)  # WM_RBUTTONUP
    time.sleep(0.6)

    menus = find("RecycleGlassMenu")
    if not menus:
        print("[FAIL] menu did not open")
        user32.SetCursorPos(*origin)
        raise SystemExit(1)
    # the panel hides itself from capture (WDA_EXCLUDEFROMCAPTURE) so its own
    # pixels never feed back into the backdrop; turn that off just for this
    # screenshot, otherwise BitBlt would not see the panel at all
    try:
        user32.SetWindowDisplayAffinity.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        user32.SetWindowDisplayAffinity(menus[0], 0)
        time.sleep(0.15)
    except Exception as e:
        print("  (could not disable capture exclusion:", e, ")")

    r = w32.RECT()
    user32.GetWindowRect(menus[0], ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    img = capture(r.left - 12, r.top - 12, w + 24, h + 24)
    img.save(out)
    print(f"[ok] menu {w}x{h} at ({r.left},{r.top}) -> {out}")
    print("     distinct colours:", len(img.getcolors(maxcolors=400000) or []))

    user32.PostMessageW(menus[0], 0x0010, 0, 0)  # WM_CLOSE
    time.sleep(0.3)
    user32.SetCursorPos(*origin)


if __name__ == "__main__":
    main()
