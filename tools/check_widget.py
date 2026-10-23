"""Verify the widget is really on screen and capture its own pixels.

    python tools/check_widget.py [out.png]

BitBlt cannot see DWM-composited windows on this machine, so the only honest
checks are PrintWindow (window content) + WindowFromPoint (hit-testing).
"""
from __future__ import annotations

import ctypes
import sys
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
user32.PrintWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
user32.IsWindowVisible.restype = ctypes.c_int
user32.GetWindowLongPtrW.restype = ctypes.c_void_p
user32.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]

GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_VISIBLE = 0x10000000
WS_EX_LAYERED = 0x00080000
PW_RENDERFULLCONTENT = 2


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_ushort),
                ("biBitCount", ctypes.c_ushort), ("biCompression", ctypes.c_uint),
                ("biSizeImage", ctypes.c_uint), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint),
                ("biClrImportant", ctypes.c_uint)]


order: list[tuple[str, int]] = []
widgets: list[int] = []


@w32.ENUMPROC
def cb(hw, lp):
    b = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hw, b, 256)
    if b.value in ("RecycleGlassWidget", "Progman", "WorkerW"):
        order.append((b.value, hw))
        if b.value == "RecycleGlassWidget":
            widgets.append(hw)
    return True


user32.EnumWindows(cb, 0)
print("[1] z-order (top->bottom):", [o[0] for o in order][:12])
if not widgets:
    print("[FAIL] no widget window found")
    raise SystemExit(1)

hw = widgets[0]
r = w32.RECT()
user32.GetWindowRect(hw, ctypes.byref(r))
w, h = r.right - r.left, r.bottom - r.top
style = user32.GetWindowLongPtrW(hw, GWL_STYLE) or 0
ex = user32.GetWindowLongPtrW(hw, GWL_EXSTYLE) or 0
print(f"[2] rect=({r.left},{r.top}) size={w}x{h}")
print(f"[3] visible={bool(user32.IsWindowVisible(hw))} "
      f"WS_VISIBLE={'on' if style & WS_VISIBLE else 'OFF'} "
      f"LAYERED={'on' if ex & WS_EX_LAYERED else 'off'}")

pt = w32.POINT(r.left + w // 2, r.top + h // 2)
hit = user32.WindowFromPoint(pt)
print(f"[4] WindowFromPoint(centre) == self: {hit == hw}")

screen = user32.GetDC(None)
mem = gdi32.CreateCompatibleDC(screen)
bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
gdi32.SelectObject(mem, bmp)
gdi32.BitBlt(mem, 0, 0, w, h, screen, 0, 0, 0x00FFFFFF)
ok = user32.PrintWindow(hw, mem, PW_RENDERFULLCONTENT)
bmi = BITMAPINFOHEADER()
bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bmi.biWidth = w
bmi.biHeight = -h
bmi.biPlanes = 1
bmi.biBitCount = 32
buf = ctypes.create_string_buffer(w * h * 4)
gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
user32.ReleaseDC(None, screen)

img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
alphas = img.getchannel("A").getextrema()
print(f"[5] PrintWindow={ok} alpha_range={alphas}")
out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "shot_widget.png"
img.save(out)
print(f"[6] saved {out}")

# how many distinct pixels? an empty surface is a single flat colour
colors = img.convert("RGB").getcolors(maxcolors=100000)
print(f"[7] distinct colours: {len(colors) if colors else '>100000'}")
