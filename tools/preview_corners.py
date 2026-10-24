"""离屏渲染卡片与菜单，用来对比 n=2（圆角）与 n=3（超椭圆）。

用法： python tools/preview_corners.py
产物： preview_corners.png
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image, ImageDraw, ImageFilter

from recycle_glass import menu as M
from recycle_glass import render as R
from recycle_glass import squircle


def wallpaper(w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h), (18, 20, 34))
    d = ImageDraw.Draw(img, "RGBA")
    for cx, cy, rad, col in [
        (w * 0.12, h * 0.10, w * 0.42, (64, 110, 255, 210)),
        (w * 0.86, h * 0.18, w * 0.36, (168, 120, 255, 190)),
        (w * 0.62, h * 0.95, w * 0.46, (40, 190, 220, 150)),
        (w * 0.30, h * 0.78, w * 0.30, (255, 150, 200, 110)),
    ]:
        d.ellipse((cx - rad, cy - rad, cx + rad, cy + rad), fill=col)
    return img.filter(ImageFilter.GaussianBlur(46))


def card(theme: str, wp: Image.Image):
    R.set_scale(1.0)
    pal = R.palette_for(theme)
    w = R.CARD_W + R.PAD * 2
    h = R.CARD_H + R.PAD * 2
    back = wp.crop((0, 0, w, h)).filter(ImageFilter.GaussianBlur(18))
    img, _ = R.render((w, h), back, R.load_app_icon(), "回收站", "128 项 · 1.24 GB",
                      False, pal, 0.86, 0.5, hover_t=0.0)
    from recycle_glass import i18n
    return img, (w, h)


def menu_img(theme: str, wp: Image.Image):
    M.set_scale(1.0)
    items = M.default_items(autostart=False, empty=False)
    dark = theme == "dark"
    back = wp.crop((0, 0, 260, 360)).filter(ImageFilter.GaussianBlur(16))
    img, _, _ = M.render_menu(items, dark, hover=2, status="128 项 · 1.24 GB",
                              app_icon=R.load_app_icon(), settings={"autostart": 0},
                              backdrop=back, header=True)
    return img


def main():
    wp = wallpaper(1100, 620)
    sheet = wp.copy()
    d = ImageDraw.Draw(sheet, "RGBA")

    variants = [(2.0, "n = 2  圆弧角"), (3.0, "n = 3  超椭圆")]
    x = 40
    for n, label in variants:
        squircle.N = n
        for theme, dy in (("light", 70), ("dark", 300)):
            img, (w, h) = card(theme, wp)
            sheet.paste(img, (x, dy), img)
        d.text((x, 40), label, fill=(255, 255, 255, 235))
        d.text((x, 262), label, fill=(255, 255, 255, 235))
        x += 520

    sheet.save("preview_corners_cards.png")

    # 菜单：两种幂次并排、两种主题
    ms = Image.new("RGB", (700, 820), (20, 22, 36))
    for i, n in enumerate([2.0, 3.0]):
        squircle.N = n
        for j, theme in enumerate(["light", "dark"]):
            m = menu_img(theme, wp)
            ms.paste(wp.crop((0, 0, 340, 400)), (i * 350, j * 410))
            ms.paste(m, (i * 350 + 10, j * 410 + 10), m)
    ms.save("preview_corners_menus.png")
    print("written preview_corners_cards.png / preview_corners_menus.png")


if __name__ == "__main__":
    main()
