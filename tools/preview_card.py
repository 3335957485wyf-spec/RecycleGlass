"""Render the card offscreen at 2x for visual review.

    python tools/preview_card.py

Writes preview_card_*.png next to this script (no window is created).
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

from recycle_glass import i18n, render, wallpaper  # noqa: E402
from recycle_glass.bin import format_count, format_size  # noqa: E402


def sheet(theme: str, tag: str) -> None:
    i18n.set_language("zh")
    render.set_scale(2.0)
    pal = render.palette_for(theme)
    icon = render.load_app_icon()
    w, h = render.WINDOW_W, render.WINDOW_H

    # real wallpaper behind the card, blurred exactly like the app does it
    bd = wallpaper.blurred_backdrop((40, 40, 40 + render.CARD_W, 40 + render.CARD_H),
                                    radius=24.0)

    frames = []
    for name, kw in (
        ("idle", dict(hover=False, hover_t=0.0)),
        ("hover", dict(hover=True, hover_t=1.0, hover_btn="empty")),
        ("mid", dict(hover=True, hover_t=0.45)),
        ("drop", dict(hover=True, hover_t=1.0, drag_hover=True)),
    ):
        img, hits = render.render(
            (w, h), bd, icon,
            i18n.t("app_name"),
            i18n.t("sub_count", n=format_count(115), size=format_size(932_000_000)),
            kw.pop("hover"), pal, 0.78, 0.55,
            hover_buttons=(i18n.t("btn_open"), i18n.t("btn_empty")),
            fx_t=0.35 if name == "drop" else None,
            **kw,
        )
        frames.append((name, img, hits))

    # compose on a neutral studio background so the shadow reads
    cols = len(frames)
    gap = 40
    W = cols * w + gap * (cols + 1)
    H = h + gap * 2
    bg = Image.new("RGB", (W, H), (238, 240, 245) if theme == "light" else (24, 26, 34))
    for i, (_n, img, _h) in enumerate(frames):
        bg.paste(img, (gap + i * (w + gap), gap), img)
    out = ROOT / f"preview_card_{tag}.png"
    bg.save(out)
    print("wrote", out, [f[0] for f in frames])

    # hit rects sanity check
    for name, _img, hits in frames:
        if hits:
            print(f"  {name}: hits={hits}")


if __name__ == "__main__":
    sheet("dark", "dark")
    sheet("light", "light")
