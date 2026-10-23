"""Build a before/after sheet: the 1.0 card PNGs vs the 2.0 renderer.

    python tools/make_compare.py

Both cards are normalised to the same width so the comparison is about
design, not about resolution.
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

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

from recycle_glass import i18n, render, wallpaper  # noqa: E402

CARD_W = 302
FONT = "C:/Windows/Fonts/msyh.ttc"


def _font(sz: int):
    try:
        return ImageFont.truetype(FONT, sz, index=1)
    except OSError:
        return ImageFont.load_default()


def desktop_slice(w: int, h: int) -> Image.Image:
    wp = wallpaper.wallpaper_path()
    base = Image.open(wp).convert("RGB")
    sw, sh = 2560, 1600
    k = max(sw / base.width, sh / base.height)
    img = base.resize((int(base.width * k), int(base.height * k)), Image.LANCZOS)
    ox, oy = (img.width - sw) // 2, (img.height - sh) // 2
    img = img.crop((ox, oy, ox + sw, oy + sh))
    return img.crop((sw - w - 160, sh - h - 200, sw - 160, sh - 200))


def _fit(img: Image.Image, width: int) -> Image.Image:
    if img.width == 0:
        return img
    h = max(1, int(round(img.height * width / img.width)))
    return img.resize((width, h), Image.LANCZOS)


def old_card(theme: str) -> Image.Image:
    name = "card_idle.png" if theme == "dark" else "light_card_idle.png"
    im = Image.open(ROOT / name).convert("RGBA")
    box = im.getchannel("A").getbbox()
    if box:
        im = im.crop(box)
    return _fit(im, CARD_W)


def new_card(theme: str) -> Image.Image:
    i18n.set_language("zh")
    render.set_scale(1.0)
    W, H = render.WINDOW_W, render.WINDOW_H
    bd = wallpaper.blurred_backdrop((0, 0, render.CARD_W, render.CARD_H), 24.0)
    card, _ = render.render(
        (W, H), bd, render.load_app_icon(), i18n.t("app_name"),
        "19263 项 · 4.99 GB", False, render.palette_for(theme), 0.78, 0.55,
        hover_buttons=(i18n.t("btn_open"), i18n.t("btn_empty")),
        hover_t=0.0)
    p = render.PAD
    return card.crop((p, p, p + render.CARD_W, p + render.CARD_H))


def main() -> None:
    cell_w, cell_h = 420, 200
    pad, top = 20, 112
    sheet = Image.new("RGB", (pad + 2 * (cell_w + pad), top + 2 * (cell_h + pad)),
                      (16, 18, 24))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 16), "RecycleGlass 1.0  →  2.0", font=_font(28),
           fill=(242, 244, 250))
    d.text((pad, 56), "同一张壁纸 · 同一尺寸（卡片宽 302px）",
           font=_font(15), fill=(138, 144, 160))

    for c, txt in enumerate(("1.0（旧）", "2.0（新）")):
        d.text((pad + c * (cell_w + pad), top - 28), txt, font=_font(17),
               fill=(160, 168, 184))

    for r, theme in enumerate(("dark", "light")):
        bg = desktop_slice(cell_w, cell_h)
        for c in range(2):
            cell = bg.copy()
            card = old_card(theme) if c == 0 else new_card(theme)
            cell.paste(card, ((cell_w - card.width) // 2,
                              (cell_h - card.height) // 2), card)
            sheet.paste(cell, (pad + c * (cell_w + pad), top + r * (cell_h + pad)))

    out = ROOT / "compare_before_after.png"
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
