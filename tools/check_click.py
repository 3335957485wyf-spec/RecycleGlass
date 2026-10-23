"""Click the "empty" pill and capture the confirmation flyout.

    python tools/check_click.py [out.png]

Safe by design: it only opens the confirmation panel and then closes it, it
never confirms the deletion.
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

from recycle_glass import i18n, render, w32  # noqa: E402
from recycle_glass.w32 import user32  # noqa: E402

gdi32 = ctypes.WinDLL("gdi32")
for name, restype, argtypes in (
    ("GetDC", ctypes.c_void_p, [ctypes.c_void_p]),
    ("ReleaseDC", ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p]),
):
    fn = getattr(user32, name)
    fn.restype = restype
    fn.argtypes = argtypes
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
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "shot_confirm.png"
    widgets = find("RecycleGlassWidget")
    if not widgets:
        print("[FAIL] widget not running")
        raise SystemExit(1)
    hw = widgets[0]

    # where are the pills?  (same maths the renderer uses)
    from recycle_glass.config import Config
    from recycle_glass.menu import SIZE_SCALES
    cfg = Config.load()
    render.set_scale(SIZE_SCALES.get(cfg.size, 1.0))
    _img, hits = render.render(
        (render.WINDOW_W, render.WINDOW_H), None, render.load_app_icon(),
        i18n.t("app_name"), "x", True, render.palette_for(cfg.theme),
        cfg.opacity, cfg.material,
        hover_buttons=(i18n.t("btn_open"), i18n.t("btn_empty")),
        hover_t=1.0)
    empty = [h for h in hits if h[0] == i18n.t("btn_empty")]
    if not empty:
        print("[FAIL] no empty pill rect")
        raise SystemExit(1)
    _l, x0, y0, x1, y1 = empty[0]
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    print(f"pill 清空 at client ({cx},{cy}) rect={empty[0][1:]}")

    lp = (cy << 16) | cx
    instant = "--instant" in sys.argv
    user32.PostMessageW(hw, 0x0200, 0, lp)          # hover
    time.sleep(0.0 if instant else 0.30)
    user32.PostMessageW(hw, 0x0201, 0x0001, lp)     # L down
    time.sleep(0.05)
    user32.PostMessageW(hw, 0x0202, 0, lp)          # L up  -> confirm flyout
    time.sleep(0.60)

    menus = find("RecycleGlassMenu")
    if not menus:
        print("[FAIL] confirmation did not open")
        raise SystemExit(1)
    r = w32.RECT()
    user32.GetWindowRect(menus[0], ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    img = capture(r.left - 14, r.top - 14, w + 28, h + 28)
    img.save(out)
    print(f"[ok] confirm {w}x{h} at ({r.left},{r.top}) -> {out}")

    # cancel — never confirm a deletion from a test script
    user32.PostMessageW(menus[0], 0x0010, 0, 0)
    time.sleep(0.3)
    print("     cancelled")


if __name__ == "__main__":
    main()
