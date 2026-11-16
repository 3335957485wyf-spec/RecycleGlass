"""Fluent context menu — reference-derived geometry, acrylic material.

Design notes
-----------
* Geometry replicated from the reference capture (158 px panel) and scaled
  1.4x: item 50, icon 22, text inset 62, chevron inset 27.
* The panel uses the same material pipeline as the recycle-bin card:
  blurred wallpaper + tint, so appearance settings stay visually consistent.
* Every rounded shape is drawn 4x and downsampled — PIL's ImageDraw has no
  anti-aliasing, so a plain rounded_rectangle leaves hard 1-bit corners.

Performance
-----------
* Outside clicks: SetCapture + a 30 ms global key-state poll (no WH_MOUSE_LL
  hook, which would intercept every system mouse event and stall input).
* Hover states are pre-rendered to BGRA bytes once per item.
* Masks / patches / border rings are cached per size.
"""
from __future__ import annotations

import ctypes
import time
from dataclasses import dataclass
from ctypes import wintypes

from PIL import Image, ImageChops, ImageDraw, ImageFilter

import os

import math

from . import anim
from . import i18n
from . import squircle
from . import screencap
from .icons import draw_icon
from . import w32
from . import wallpaper
from .w32 import user32, kernel32, WNDCLASSEXW, WNDPROC, POINT
from . import desktop
from . import screens
from .desktop import update_layered
from .render import _font, to_bgra

# ---------------------------------------------------------------- metrics
# All derived from BASE * SCALE so the menu follows the same DPI / size preset
# as the card.
BASE = {
    "MENU_W": 316, "ITEM_H": 50, "PAD_TOP": 6, "PAD_BOTTOM": 6,
    "SEP_BLOCK": 6, "RADIUS": 15, "ICON_LEFT": 22, "ICON_SIZE": 24,
    "TEXT_LEFT": 62, "CHEV_INSET": 27, "ITEM_RADIUS": 8, "HOVER_INSET": 4,
    "BODY_FONT_SIZE": 20, "SET_FONT_SIZE": 17,
    "SET_ROW_H": 48, "SET_INDENT": 22, "SET_LABEL_W": 78,
    "SET_VALUE_W": 56, "SEG_H": 31,
    "HEADER_H": 78, "HEADER_ICON": 46,
}

MENU_W = ITEM_H = PAD_TOP = PAD_BOTTOM = SEP_BLOCK = RADIUS = 0
ICON_LEFT = ICON_SIZE = TEXT_LEFT = CHEV_INSET = ITEM_RADIUS = HOVER_INSET = 0
BODY_FONT_SIZE = SET_FONT_SIZE = 0
SET_ROW_H = SET_INDENT = SET_LABEL_W = SET_VALUE_W = SEG_H = 0
HEADER_H = HEADER_ICON = 0


def set_scale(scale: float) -> None:
    """Rebuild every menu metric for the given scale factor."""
    global MENU_W, ITEM_H, PAD_TOP, PAD_BOTTOM, SEP_BLOCK, RADIUS
    global ICON_LEFT, ICON_SIZE, TEXT_LEFT, CHEV_INSET, ITEM_RADIUS, HOVER_INSET
    global BODY_FONT_SIZE, SET_FONT_SIZE
    global SET_ROW_H, SET_INDENT, SET_LABEL_W, SET_VALUE_W, SEG_H
    global HEADER_H, HEADER_ICON

    s = max(0.6, min(2.5, float(scale)))

    def k(name: str, lo: int) -> int:
        return max(lo, int(round(BASE[name] * s)))

    MENU_W = k("MENU_W", 190)
    ITEM_H = k("ITEM_H", 30)
    PAD_TOP = k("PAD_TOP", 3)
    PAD_BOTTOM = k("PAD_BOTTOM", 3)
    SEP_BLOCK = k("SEP_BLOCK", 3)
    RADIUS = k("RADIUS", 8)
    ICON_LEFT = k("ICON_LEFT", 12)
    ICON_SIZE = k("ICON_SIZE", 14)
    TEXT_LEFT = k("TEXT_LEFT", 34)
    CHEV_INSET = k("CHEV_INSET", 14)
    ITEM_RADIUS = k("ITEM_RADIUS", 4)
    HOVER_INSET = k("HOVER_INSET", 2)
    BODY_FONT_SIZE = k("BODY_FONT_SIZE", 12)
    SET_FONT_SIZE = k("SET_FONT_SIZE", 10)
    SET_ROW_H = k("SET_ROW_H", 28)
    SET_INDENT = k("SET_INDENT", 12)
    SET_LABEL_W = k("SET_LABEL_W", 44)
    SET_VALUE_W = k("SET_VALUE_W", 32)
    SEG_H = k("SEG_H", 18)
    HEADER_H = k("HEADER_H", 46)
    HEADER_ICON = k("HEADER_ICON", 28)


set_scale(1.0)

TIMER_WATCH = 1
WATCH_MS = 30

TIMER_ANIM = 2
ANIM_MS = 5              # 200 Hz tick (needs anim.begin() for 1 ms timers)
ANIM_IN = 0.20           # fade in + slide up (slower, reads as a settle)
ANIM_OUT = 0.14          # fade out
ANIM_DY_IN = 10          # px travelled upward on entry
ANIM_DY_OUT = 6          # px drifted downward on exit
EXPAND_DUR = 0.20        # height reveal when the settings panel opens

_SS = 4  # supersampling for masks / rounded patches
_ICON_SS = 16  # icons get many samples: 24 px is unforgiving


@dataclass
class MenuItem:
    label: str
    icon: str
    cmd: int
    separator: bool = False
    checked: bool = False
    disabled: bool = False
    chevron: bool = False
    expandable: bool = False
    destructive: bool = False
    inline_toggle: bool = False


def default_items(autostart: bool, empty: bool = True,
                  auto_dpi: bool = False) -> list[MenuItem]:
    return [
        MenuItem(i18n.t("menu_open"), "folder", 101),
        MenuItem(i18n.t("menu_empty"), "trash", 102, disabled=empty,
                 destructive=True),
        MenuItem("", "", 0, separator=True),
        MenuItem(i18n.t("menu_appearance"), "sliders", 103, chevron=True,
                 expandable=True),
        MenuItem(i18n.t("menu_autostart"),
                 "check" if autostart else "checkbox_off", 104,
                 checked=autostart, inline_toggle=True),
        MenuItem(i18n.t("menu_dpi"), "check" if auto_dpi else "checkbox_off",
                 106, checked=auto_dpi, inline_toggle=True),
        MenuItem("", "", 0, separator=True),
        MenuItem(i18n.t("menu_exit"), "power", 105, destructive=True),
    ]


def confirm_items() -> list[MenuItem]:
    """Two-row flyout used as the "empty recycle bin?" prompt."""
    return [
        MenuItem(i18n.t("confirm_cancel"), "close", 201),
        MenuItem(i18n.t("menu_empty"), "trash", 202, destructive=True),
    ]


def theme_options():
    return (("system", i18n.t("opt_system")),
            ("light", i18n.t("opt_light")),
            ("dark", i18n.t("opt_dark")))


def size_options():
    return (("small", i18n.t("opt_small")),
            ("normal", i18n.t("opt_normal")),
            ("large", i18n.t("opt_large")))


def set_sliders():
    return (
        ("opacity", i18n.t("set_opacity"), 20, 100, "%"),
        ("material", i18n.t("set_material"), 0, 100, "%"),
        ("blur", i18n.t("set_blur"), 0, 60, ""),
    )


SIZE_SCALES = {"small": 0.85, "normal": 1.0, "large": 1.18}


# ---------------------------------------------------------------- icons

_SPRITES: dict[tuple, Image.Image] = {}


_HEADER_ICONS: dict[tuple, Image.Image] = {}


def _header_icon(icon: Image.Image, size: int) -> Image.Image:
    key = (id(icon), size)
    got = _HEADER_ICONS.get(key)
    if got is None:
        got = icon.resize((size, size), Image.Resampling.LANCZOS)
        _HEADER_ICONS[key] = got
    return got


def icon_sprite(key: str, size: int, rgb, alpha: int = 255,
                rotate: float = 0.0) -> Image.Image:
    q = round(rotate / 5.0) * 5.0          # 5° steps keep the cache small
    cache_key = ("icon", key, size, tuple(rgb), alpha, q)
    got = _SPRITES.get(cache_key)
    if got is not None:
        return got
    box = size * _ICON_SS
    big = Image.new("RGBA", (box, box), (0, 0, 0, 0))
    # Lucide's default stroke is 2 units on a 24-unit grid
    draw_icon(ImageDraw.Draw(big), key, box, box / 2.0, box / 2.0,
              (*rgb, alpha), box * (2.0 / 24.0), rotate=q)
    got = big.resize((size, size), Image.Resampling.LANCZOS)
    _SPRITES[cache_key] = got
    return got


# ------------------------------------------------- anti-aliased primitives

_MASK_CACHE: dict[tuple, Image.Image] = {}
_PATCH_CACHE: dict[tuple, Image.Image] = {}


def _aa_mask(w: int, h: int, radius: int, inset: int = 0) -> Image.Image:
    """Anti-aliased rounded-rect coverage mask (0..255)."""
    key = ("mask", w, h, radius, inset)
    got = _MASK_CACHE.get(key)
    if got is not None:
        return got
    big = Image.new("L", (w * _SS, h * _SS), 0)
    i = inset * _SS
    pts = squircle.curve(w * _SS - 2 * i, h * _SS - 2 * i,
                         max(0, (radius - inset)) * _SS, (i, i))
    ImageDraw.Draw(big).polygon(pts, fill=255)
    got = big.resize((w, h), Image.Resampling.LANCZOS)
    _MASK_CACHE[key] = got
    return got


def _border_layer(w: int, h: int, radius: int, rgb, alpha: int) -> Image.Image:
    """1 px AA hairline drawn just inside the panel edge."""
    key = ("border", w, h, radius, tuple(rgb), alpha)
    got = _MASK_CACHE.get(key)
    if got is not None:
        return got
    ring = ImageChops.subtract(_aa_mask(w, h, radius),
                               _aa_mask(w, h, radius, inset=1))
    a = ring.point(lambda v: v * alpha // 255)
    layer = Image.new("RGBA", (w, h), (*rgb, 0))
    layer.putalpha(a)
    _MASK_CACHE[key] = layer
    return layer


def _rim_layer(w: int, h: int, top_a: int, bottom_a: int,
               radius: int = 0, glow: float = 0.55) -> Image.Image:
    """Glass rim: bright top edge, soft sides, plus a 3-6 px inner halo.

    Measured off a reference panel: the top edge sits ~+80 luma above the
    surface, the sides ~+30, with a short falloff inside the edge.  A single
    1 px line reads flat next to that, so the rim is a hairline plus two wider
    rings at reduced alpha, all scaled by a ramp that decays away from the top.
    """
    radius = radius or RADIUS
    key = ("rim", w, h, radius, top_a, bottom_a, round(glow, 2))
    got = _MASK_CACHE.get(key)
    if got is not None:
        return got
    strip = Image.new("L", (1, h))
    for y in range(h):
        u = y / max(1, h - 1)
        a = bottom_a + (top_a - bottom_a) * math.exp(-5.5 * u)
        strip.putpixel((0, y), max(0, min(255, int(round(a)))))
    ramp = strip.resize((w, h), Image.Resampling.BILINEAR)

    # Continuous falloff: blur the hairline instead of stacking discrete rings
    ring = ImageChops.subtract(_aa_mask(w, h, radius),
                               _aa_mask(w, h, radius, inset=1))
    halo = ring.filter(ImageFilter.GaussianBlur(max(1.2, radius * 0.13)))
    halo = ImageChops.multiply(halo, _aa_mask(w, h, radius))
    cover = ImageChops.add(ring, halo.point(lambda v: int(v * glow)))

    layer = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    layer.putalpha(ImageChops.multiply(cover, ramp))
    _MASK_CACHE[key] = layer
    return layer


def _bottom_rim(w: int, alpha: int, radius: int = 0) -> Image.Image:
    """Rim for a panel cut mid-animation: bottom edge + corner arcs only.

    The strip already carries the panel's *side* rim (it is a crop of the full
    panel), so adding a full ring would double it along the sides — that extra
    bright band travelled up with the cut edge and read as a twitch.  Keeping
    only the bottom `radius` rows leaves just the arc that is actually missing.
    """
    radius = radius or RADIUS
    key = ("brim", w, radius, alpha)
    got = _MASK_CACHE.get(key)
    if got is not None:
        return got
    hh = radius * 2
    ring = ImageChops.subtract(_aa_mask(w, hh, radius),
                               _aa_mask(w, hh, radius, inset=1))
    # keep only the rows where the bottom arc lives: rows [radius, 2*radius).
    # Including even 2 px of the straight side above the corner made a tiny
    # bright segment that travelled up with the cut edge — that was the
    # "white highlight flicker" at the bottom corners while collapsing.
    ring = ring.crop((0, radius, w, hh))
    a = ring.point(lambda v: int(v * alpha / 255))
    layer = Image.new("RGBA", (w, ring.height), (255, 255, 255, 0))
    layer.putalpha(a)
    _MASK_CACHE[key] = layer
    return layer


def _aa_patch(w: int, h: int, radius: int, color) -> Image.Image:
    """Anti-aliased rounded rectangle in `color` (RGBA)."""
    key = ("patch", w, h, radius, tuple(color))
    got = _PATCH_CACHE.get(key)
    if got is not None:
        return got
    big = Image.new("RGBA", (w * _SS, h * _SS), (0, 0, 0, 0))
    pts = squircle.polygon(w * _SS, h * _SS, radius * _SS)
    ImageDraw.Draw(big).polygon(pts, fill=color)
    got = big.resize((w, h), Image.Resampling.LANCZOS)
    _PATCH_CACHE[key] = got
    return got


def _aa_disc(diameter: int, color) -> Image.Image:
    key = ("disc", diameter, tuple(color))
    got = _PATCH_CACHE.get(key)
    if got is not None:
        return got
    d = diameter
    big = Image.new("RGBA", (d * _SS, d * _SS), (0, 0, 0, 0))
    ImageDraw.Draw(big).ellipse((0, 0, d * _SS - 1, d * _SS - 1), fill=color)
    got = big.resize((d, d), Image.Resampling.LANCZOS)
    _PATCH_CACHE[key] = got
    return got


# ---------------------------------------------------------------- palette

def _clamp01(v: float) -> float:
    return max(0.0, min(1.0, v))


def _palette(dark: bool, settings=None):
    """Colours derived from the same appearance settings as the card.

    `material` drives how strongly the tint covers the blurred wallpaper and
    `opacity` drives the panel's own alpha, so the menu tracks the card.
    """
    st = settings or {}
    op = _clamp01(float(st.get("opacity", 90)) / 100.0)
    mat = _clamp01(float(st.get("material", 45)) / 100.0)
    strength = 0.34 + 0.46 * mat
    panel_alpha = int(round(255 * (0.74 + 0.26 * op)))

    if dark:
        return {
            "bg_rgb": (38, 40, 52),
            "tint": (38, 40, 52),
            "tint_strength": strength,
            "panel_alpha": panel_alpha,
            "border": (255, 255, 255, 34),
            "rim": (2, 196, 64),
            "text": (226, 226, 232),
            "dim": (160, 162, 176),
            "disabled": (116, 118, 132),
            "hover": (255, 255, 255, 20),
            "sep": (255, 255, 255, 30),
            "accent": (126, 186, 252),
            "danger": (243, 136, 156),
            "track": (255, 255, 255, 34),
            "knob": (238, 240, 246, 255),
            "seg_bg": (255, 255, 255, 22),
            "seg_on": (126, 186, 252, 62),
        }
    # macOS Sonoma light — must stay near-opaque; a half-white tint over a
    # dark wallpaper is what made the old light theme look like grey mud.
    return {
        "bg_rgb": (250, 250, 252),
        "tint": (250, 250, 252),
        "tint_strength": 0.50 + 0.42 * mat,
        "panel_alpha": int(round(255 * (0.68 + 0.32 * op))),
        "border": (0, 0, 0, 28),
        "rim": (2, 205, 82),
        "text": (29, 29, 31),
        "dim": (95, 95, 103),
        "disabled": (152, 152, 160),
        "hover": (0, 0, 0, 11),
        "sep": (0, 0, 0, 16),
        "accent": (0, 122, 255),
        "danger": (207, 59, 54),
        "track": (0, 0, 0, 22),
        "knob": (255, 255, 255, 255),
        "seg_bg": (0, 0, 0, 10),
        "seg_on": (0, 122, 255, 34),
    }


def _slider_geometry():
    x0 = SET_INDENT + SET_LABEL_W
    x1 = MENU_W - SET_INDENT - SET_VALUE_W
    return x0, x1


def menu_size(items, expanded: bool = False, header: bool = False) -> tuple[int, int]:
    h = PAD_TOP
    if header:
        h += HEADER_H + SEP_BLOCK
    for it in items:
        h += SEP_BLOCK if it.separator else ITEM_H
    if expanded:
        h += SEP_BLOCK + SET_ROW_H * (len(set_sliders()) + 2)
    h += PAD_BOTTOM
    return MENU_W, h


# ---------------------------------------------------------------- render

def render_menu(items, dark, hover=-1, status="", app_icon=None,
                expanded=False, settings=None, backdrop=None,
                header: bool = False, title: str | None = None,
                chevron_t: float | None = 0.0, divider_alpha: float = 1.0):
    """Return (RGBA image, hit_rects, controls)."""
    st = settings or {}
    pal = _palette(dark, st)
    w, h = menu_size(items, expanded, header=header)

    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    mask = _aa_mask(w, h, RADIUS)
    panel_a = pal["panel_alpha"]
    if panel_a < 255:
        mask = mask.point(lambda v, a=panel_a: v * a // 255)

    if backdrop is not None:
        body = backdrop.convert("RGBA")
        if body.size != (w, h):
            body = body.resize((w, h), Image.Resampling.BILINEAR)
        overlay = Image.new("RGBA", (w, h), (*pal["tint"], 255))
        body = Image.blend(body, overlay, pal["tint_strength"])
        body.putalpha(mask)
        img.alpha_composite(body)
    else:
        body = Image.new("RGBA", (w, h), (*pal["bg_rgb"], 255))
        body.putalpha(mask)
        img.alpha_composite(body)

    rim = pal.get("rim")
    if rim:
        th, ta, ba = rim
        img.alpha_composite(_rim_layer(w, h, ta, ba))
    else:
        img.alpha_composite(_border_layer(w, h, RADIUS,
                                          pal["border"][:3], pal["border"][3]))
    d = ImageDraw.Draw(img)

    body_font = _font(BODY_FONT_SIZE)
    set_font = _font(SET_FONT_SIZE)

    def _dy(font, box_h):
        bb = font.getbbox("国")
        return (box_h - (bb[3] - bb[1])) // 2 - bb[1]

    body_dy = _dy(body_font, ITEM_H)
    set_dy = _dy(set_font, SET_ROW_H)

    hits = []
    y = PAD_TOP
    if header:
        # app icon + name + live status — the flyout header Win11 uses for
        # its own shell flyouts, so the panel reads as part of the system
        icon_sz = HEADER_ICON
        gap = max(10, int(round(icon_sz * 0.32)))
        if app_icon is not None:
            img.alpha_composite(_header_icon(app_icon, icon_sz),
                                (ICON_LEFT, y + (HEADER_H - icon_sz) // 2))
        else:
            img.alpha_composite(
                _aa_patch(icon_sz, icon_sz, int(icon_sz * 0.26),
                          pal["seg_bg"]),
                (ICON_LEFT, y + (HEADER_H - icon_sz) // 2))
        tx = ICON_LEFT + icon_sz + gap
        tf = _font(BODY_FONT_SIZE, bold=True)
        name = title or i18n.t("app_name")
        # shrink the status line until it fits instead of letting it run off
        # the panel (English / long counts are the usual offenders)
        avail = max(40, w - tx - SET_INDENT)
        sf = _font(SET_FONT_SIZE)
        for fs in range(SET_FONT_SIZE, 9, -1):
            sf = _font(fs)
            if sf.getbbox(status or " ")[2] <= avail:
                break
        tb = tf.getbbox(name)
        sb = sf.getbbox(status or " ")
        th, sh = tb[3] - tb[1], sb[3] - sb[1]
        vgap = max(2, int(round(4 * (HEADER_H / 78.0))))
        block = th + vgap + sh
        top = y + (HEADER_H - block) // 2
        d.text((tx, top - tb[1]), name, font=tf, fill=pal["text"])
        d.text((tx, top + th + vgap - sb[1]), status, font=sf, fill=pal["dim"])
        d = ImageDraw.Draw(img)
        ly = y + HEADER_H + SEP_BLOCK // 2
        ov = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(ov).line((HOVER_INSET, ly, w - 1 - HOVER_INSET, ly),
                                fill=pal["sep"])
        img.alpha_composite(ov)
        d = ImageDraw.Draw(img)
        y += HEADER_H + SEP_BLOCK

    for idx, it in enumerate(items):
        if it.separator:
            ly = y + SEP_BLOCK // 2
            ov = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            ImageDraw.Draw(ov).line((HOVER_INSET, ly, w - 1 - HOVER_INSET, ly),
                                    fill=pal["sep"])
            img.alpha_composite(ov)
            d = ImageDraw.Draw(img)
            y += SEP_BLOCK
            continue

        row = (0, y, w - 1, y + ITEM_H)
        active = (idx == hover and not it.disabled) or (it.expandable and expanded)
        if active:
            img.alpha_composite(
                _aa_patch(w - 2 * HOVER_INSET, ITEM_H, ITEM_RADIUS, pal["hover"]),
                (HOVER_INSET, y))
            d = ImageDraw.Draw(img)

        cy = y + ITEM_H // 2
        if it.disabled:
            fg, alpha = pal["disabled"], 170
        elif it.destructive:
            fg, alpha = pal["danger"], 255
        else:
            fg, alpha = pal["text"], 255
        icon_fg = pal["accent"] if it.checked and not it.disabled else fg

        img.alpha_composite(icon_sprite(it.icon, ICON_SIZE, icon_fg, alpha),
                            (ICON_LEFT, cy - ICON_SIZE // 2))
        d = ImageDraw.Draw(img)

        d.text((TEXT_LEFT, y + body_dy), it.label, font=body_font, fill=fg)

        if it.chevron and chevron_t is not None:
            cs = 16
            img.alpha_composite(
                icon_sprite("chevron", cs, pal["dim"], 255,
                            rotate=90.0 * chevron_t),
                (w - CHEV_INSET - cs // 2, cy - cs // 2))
            d = ImageDraw.Draw(img)

        hits.append((idx, *row))
        y += ITEM_H

    controls = []
    if expanded:
        ly = y + SEP_BLOCK // 2
        sep = pal["sep"]
        if divider_alpha < 0.999:
            sep = (*sep[:3], int(sep[3] * max(0.0, divider_alpha)))
        ov = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(ov).line((HOVER_INSET, ly, w - 1 - HOVER_INSET, ly),
                                fill=sep)
        img.alpha_composite(ov)
        d = ImageDraw.Draw(img)
        y += SEP_BLOCK

        tx0, tx1 = _slider_geometry()
        knob_d = 14
        track_w = tx1 - tx0 + 1

        for key, label, vmin, vmax, suffix in set_sliders():
            d.text((SET_INDENT, y + set_dy), label, font=set_font, fill=pal["dim"])

            tcy = y + SET_ROW_H // 2
            img.alpha_composite(_aa_patch(track_w, 6, 3, pal["track"]),
                                (tx0, tcy - 3))
            val = float(st.get(key, vmin))
            frac = 0.0 if vmax == vmin else (val - vmin) / (vmax - vmin)
            frac = max(0.0, min(1.0, frac))
            kx = int(tx0 + frac * (tx1 - tx0))
            if kx > tx0:
                img.alpha_composite(
                    _aa_patch(max(6, kx - tx0 + 1), 6, 3, pal["accent"]),
                    (tx0, tcy - 3))
            img.alpha_composite(_aa_disc(knob_d, pal["knob"]),
                                (kx - knob_d // 2, tcy - knob_d // 2))
            d = ImageDraw.Draw(img)

            vtxt = f"{int(round(val))}{suffix}"
            vw = int(d.textlength(vtxt, font=set_font))
            d.text((w - SET_INDENT - vw, y + set_dy), vtxt,
                   font=set_font, fill=pal["dim"])

            controls.append(("slider", key,
                             (tx0 - 12, y, tx1 + 12, y + SET_ROW_H),
                             (vmin, vmax)))
            y += SET_ROW_H

        d.text((SET_INDENT, y + set_dy), i18n.t("set_theme"), font=set_font,
               fill=pal["dim"])
        cur = st.get("theme", "system")
        theme_opts = theme_options()
        n = len(theme_opts)
        # the three options split the full row evenly (no unused value column)
        seg_left = tx0
        seg_right = MENU_W - SET_INDENT
        seg_gap = 4
        seg_w = (seg_right - seg_left - seg_gap * (n - 1)) // n
        seg_y0 = y + (SET_ROW_H - SEG_H) // 2
        for i, (tkey, tlabel) in enumerate(theme_opts):
            sx0 = seg_left + i * (seg_w + seg_gap)
            on = (cur == tkey)
            img.alpha_composite(
                _aa_patch(seg_w, SEG_H, 7,
                          pal["seg_on"] if on else pal["seg_bg"]),
                (sx0, seg_y0))
            d = ImageDraw.Draw(img)
            tw = int(d.textlength(tlabel, font=set_font))
            sbb = set_font.getbbox(tlabel)
            st_y = seg_y0 + (SEG_H - (sbb[3] - sbb[1])) // 2 - sbb[1]
            d.text((sx0 + (seg_w - tw) // 2, st_y), tlabel,
                   font=set_font, fill=pal["accent"] if on else pal["dim"])
            controls.append(("seg", "theme",
                             (sx0, seg_y0, sx0 + seg_w - 1, seg_y0 + SEG_H), tkey))
        y += SET_ROW_H

        # ---- interface size ----
        d.text((SET_INDENT, y + set_dy), i18n.t("set_size"), font=set_font,
               fill=pal["dim"])
        cur_size = st.get("size", "normal")
        size_opts = size_options()
        ns = len(size_opts)
        seg_w2 = (seg_right - seg_left - seg_gap * (ns - 1)) // ns
        seg_y1 = y + (SET_ROW_H - SEG_H) // 2
        for i, (skey, slabel) in enumerate(size_opts):
            sx0 = seg_left + i * (seg_w2 + seg_gap)
            on = (cur_size == skey)
            img.alpha_composite(
                _aa_patch(seg_w2, SEG_H, 7,
                          pal["seg_on"] if on else pal["seg_bg"]),
                (sx0, seg_y1))
            d = ImageDraw.Draw(img)
            tw = int(d.textlength(slabel, font=set_font))
            sbb = set_font.getbbox(slabel)
            st_y = seg_y1 + (SEG_H - (sbb[3] - sbb[1])) // 2 - sbb[1]
            d.text((sx0 + (seg_w2 - tw) // 2, st_y), slabel,
                   font=set_font, fill=pal["accent"] if on else pal["dim"])
            controls.append(("seg", "size",
                             (sx0, seg_y1, sx0 + seg_w2 - 1, seg_y1 + SEG_H),
                             skey))
        y += SET_ROW_H

    return img, hits, controls


# ---------------------------------------------------------------- window

_hwnd_map: dict[int, "MenuWindow"] = {}
_GLOBAL_WNDPROC = None
_menu_ref = None


def _global_wnd_proc(hwnd, msg, w, l):
    inst = _hwnd_map.get(hwnd)
    if inst is not None:
        try:
            return inst._handle(hwnd, msg, w, l)
        except Exception:
            pass
    return user32.DefWindowProcW(hwnd, msg, w, l)


MENU_CLASS = "RecycleGlassMenu"


class MenuWindow:
    def __init__(self, items, on_select, dark, status_text="", app_icon=None,
                 settings=None, on_setting=None, blur_radius=24.0,
                 header_title: str | None = None):
        self.items = items
        self.on_select = on_select
        self.dark = dark
        self.status_text = status_text
        self.header_title = header_title
        self.app_icon = app_icon
        self.settings = dict(settings or {})
        self.on_setting = on_setting
        self.blur_radius = float(blur_radius)
        self._backdrop = None

        self.hover = -1
        self.hits = []
        self.controls = []
        self.expanded = False
        self.hwnd = 0
        self._open = False
        self._base_img = None
        self._base_bgra = None
        self._hover_bgra = {}
        self._fixing_capture = False
        self._drag_slider = None
        self._opened_at = 0.0
        self._origin = (0, 0)
        self._size = (MENU_W, 0)
        self._closing = False
        self._anim_in = True
        self._anim_t0 = 0.0
        self._anim_dur = 0.0
        self._anim_holding = False
        self.header = True
        # height-reveal animation for the "appearance" panel
        self._exp_anim = False
        self._exp_h0 = 0
        self._exp_h1 = 0
        self._exp_t0 = 0.0
        self._exp_dur = EXPAND_DUR

    # ---- lifecycle ----
    def popup(self, x, y, expanded: bool = False):
        global _menu_ref
        if _menu_ref and _menu_ref._open:
            _menu_ref.close(animate=False)
        if self._open:
            self.close(animate=False)
        self.expanded = bool(expanded)
        self._create(x, y)
        self._open = True
        self._closing = False
        _menu_ref = self

        # enter: fade in while sliding up
        self._anim_in = True
        self._anim_t0 = time.monotonic()
        self._anim_dur = ANIM_IN
        self._paint(move_to=(x, y))

        user32.ShowWindow(self.hwnd, 8)
        self._opened_at = time.monotonic()
        user32.SetCapture(self.hwnd)
        user32.SetTimer(self.hwnd, TIMER_WATCH, WATCH_MS, None)
        self._start_anim()

    def _create(self, x, y):
        global _GLOBAL_WNDPROC
        hinst = kernel32.GetModuleHandleW(None)
        if _GLOBAL_WNDPROC is None:
            _GLOBAL_WNDPROC = WNDPROC(_global_wnd_proc)
            wc = WNDCLASSEXW()
            wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
            wc.lpfnWndProc = ctypes.cast(_GLOBAL_WNDPROC, ctypes.c_void_p).value
            wc.hInstance = hinst
            wc.hCursor = user32.LoadCursorW(None, ctypes.c_void_p(32512))
            wc.lpszClassName = MENU_CLASS
            user32.RegisterClassExW(ctypes.byref(wc))

        w, h = menu_size(self.items, self.expanded, header=self.header)
        # keep the panel inside the work area of the monitor it opens on
        wl, wt, wr, wb = screens.work_area_at(x, y)
        if x + w > wr - 8:
            x = wr - w - 8
        if x < wl + 8:
            x = wl + 8
        if y + h > wb - 8:
            y = wb - h - 8
        if y < wt + 8:
            y = wt + 8

        self.native = False
        self._refresh_backdrop(x, y, w, h)
        self._rebuild_surface()

        ex = w32.WS_EX_TOOLWINDOW | w32.WS_EX_TOPMOST
        if not self.native:
            ex |= w32.WS_EX_LAYERED
        self.hwnd = user32.CreateWindowExW(
            ex, MENU_CLASS, "Menu", w32.WS_POPUP, x, y, w, h, 0, 0, hinst, None)
        self._size = (w, h)
        self._origin = (x, y)
        _hwnd_map[self.hwnd] = self
        # keep the panel out of its own backdrop capture (Win10 2004+);
        # RECYCLEGLASS_NO_CAPTURE_EXCLUSION=1 is for screenshots/recording
        self._exclude_ok = False
        if not os.environ.get("RECYCLEGLASS_NO_CAPTURE_EXCLUSION"):
            self._exclude_ok = screencap.exclude_from_capture(self.hwnd)

    def _refresh_backdrop(self, x, y, w, h):
        """Blur whatever is *actually* behind the panel.

        A flyout floats over documents and browsers, so sampling the wallpaper
        made text readable straight through the panel — it looked like a
        translucent sheet, not acrylic.  We capture the composited screen
        instead (excluding ourselves) and fall back to the wallpaper when the
        capture is unavailable.
        """
        rect = (int(x), int(y), int(x + w), int(y + h))
        self._backdrop = None
        hidden = False
        try:
            if self.hwnd and not getattr(self, "_exclude_ok", False):
                # no capture exclusion on this build: hide, then wait for the
                # compositor — without the flush the capture still contains the
                # panel and the backdrop turns into a copy of itself
                user32.ShowWindow(self.hwnd, 0)   # SW_HIDE
                hidden = True
                try:
                    ctypes.WinDLL("dwmapi").DwmFlush()
                except Exception:
                    pass
                time.sleep(0.015)
            self._backdrop = screencap.live_backdrop(rect,
                                                     radius=self.blur_radius)
        except Exception:
            self._backdrop = None
        finally:
            if hidden and self.hwnd:
                user32.ShowWindow(self.hwnd, 8)   # SW_SHOWNA
        if self._backdrop is None:
            try:
                self._backdrop = wallpaper.blurred_backdrop(
                    rect, radius=self.blur_radius)
            except Exception:
                self._backdrop = None
        dump = os.environ.get("RECYCLEGLASS_DUMP_BACKDROP")
        if dump and self._backdrop is not None:
            try:
                self._backdrop.save(dump)
            except Exception:
                pass

    def _rebuild_surface(self, expanded: bool | None = None,
                         chevron_t: float | None = 0.0,
                         divider_alpha: float = 1.0):
        exp = self.expanded if expanded is None else expanded
        if chevron_t == 0.0 and exp:
            chevron_t = 1.0
        self._base_img, self.hits, self.controls = render_menu(
            self.items, self.dark, -1, self.status_text, self.app_icon,
            expanded=exp, settings=self.settings,
            backdrop=self._backdrop, header=self.header,
            title=self.header_title, chevron_t=chevron_t,
            divider_alpha=divider_alpha)
        self._base_bgra = to_bgra(self._base_img)
        self._hover_bgra.clear()

    def _chevron_center(self):
        """Centre of the disclosure chevron (same walk as render_menu)."""
        y = PAD_TOP + (HEADER_H + SEP_BLOCK if self.header else 0)
        for it in self.items:
            if it.separator:
                y += SEP_BLOCK
                continue
            if it.chevron:
                return (self._size[0] - CHEV_INSET, y + ITEM_H // 2)
            y += ITEM_H
        return None

    def _expand_t(self) -> float:
        """0 = collapsed, 1 = expanded (eased)."""
        if not self._exp_anim:
            return 1.0 if self.expanded else 0.0
        u = (time.monotonic() - self._exp_t0) / max(0.001, self._exp_dur)
        u = max(0.0, min(1.0, u))
        e = 1.0 - (1.0 - u) ** 3
        return e if self._exp_h1 > self._exp_h0 else 1.0 - e

    def _chevron_center(self):
        """Centre of the disclosure chevron (same walk as render_menu)."""
        y = PAD_TOP + (HEADER_H + SEP_BLOCK if self.header else 0)
        for it in self.items:
            if it.separator:
                y += SEP_BLOCK
                continue
            if it.chevron:
                return (self._size[0] - CHEV_INSET, y + ITEM_H // 2)
            y += ITEM_H
        return None

    def _expand_t(self) -> float:
        """0 = collapsed, 1 = expanded (eased)."""
        if not self._exp_anim:
            return 1.0 if self.expanded else 0.0
        u = (time.monotonic() - self._exp_t0) / max(0.001, self._exp_dur)
        u = max(0.0, min(1.0, u))
        e = 1.0 - (1.0 - u) ** 3
        return e if self._exp_h1 > self._exp_h0 else 1.0 - e

    def _divider_y(self):
        """y of the hairline between the last item and the settings block."""
        if not self.expanded:
            return None
        y = PAD_TOP + (HEADER_H + SEP_BLOCK if self.header else 0)
        for it in self.items:
            y += SEP_BLOCK if it.separator else ITEM_H
        return y + SEP_BLOCK // 2

    def _expand_height(self) -> int:
        u = (time.monotonic() - self._exp_t0) / max(0.001, self._exp_dur)
        u = max(0.0, min(1.0, u))
        e = 1.0 - (1.0 - u) ** 3
        return int(round(self._exp_h0 + (self._exp_h1 - self._exp_h0) * e))

    def _strip_bgra(self, hh: int) -> bytes:
        """The panel cut at `hh`, with a rounded bottom edge (no shadow)."""
        w, h = self._size
        hh = max(RADIUS * 2, min(h, hh))
        img = self._base_img
        strip = img.crop((0, 0, w, hh))
        cap_h = min(hh, RADIUS)
        # take the panel's own bottom corners: a fresh rounded rect would also
        # round the top of the strip, cutting into the sides mid-panel
        cap = _aa_mask(w, h, RADIUS).crop((0, h - cap_h, w, h))
        alpha = strip.getchannel("A")
        alpha.paste(ImageChops.multiply(alpha.crop((0, hh - cap_h, w, hh)), cap),
                    (0, hh - cap_h))
        strip.putalpha(alpha)

        pal = _palette(self.dark, self.settings)
        # the cut edge needs the same rim as a real panel bottom, otherwise the
        # highlight pops when the animation lands
        rim = pal.get("rim")
        if rim:
            br = _bottom_rim(w, rim[2])
            strip.alpha_composite(br, (0, hh - br.height))
        # chevron rotates with the reveal (the base image omits it)
        c = self._chevron_center()
        if c is not None:
            cs = 16
            strip.alpha_composite(
                icon_sprite("chevron", cs, pal["dim"], 255,
                            rotate=90.0 * self._expand_t()),
                (c[0] - cs // 2, c[1] - cs // 2))

        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(strip, (0, 0))
        return to_bgra(canvas)

    def _paint(self, move_to=None):
        if not self.hwnd or self._base_bgra is None:
            return
        bgra = self._base_bgra
        if self._exp_anim:
            bgra = self._strip_bgra(self._expand_height())
        elif self.hover >= 0:
            cached = self._hover_bgra.get(self.hover)
            if cached is None:
                cached = self._render_hover(self.hover)
                if cached is not None:
                    self._hover_bgra[self.hover] = cached
            if cached is not None:
                bgra = cached
        w, h = self._size
        dy, a = self._anim_state()
        nat = getattr(self, 'native', False)
        if move_to is not None:
            desktop.present(self.hwnd, w, h, move_to[0], move_to[1] + dy,
                            bgra, alpha=a, native=nat)
        elif dy or a != 255:
            desktop.present(self.hwnd, w, h, self._origin[0],
                            self._origin[1] + dy, bgra, alpha=a, native=nat)
        else:
            desktop.present(self.hwnd, w, h, None, None, bgra, native=nat)

    def _start_anim(self):
        if not self._anim_holding:
            self._anim_holding = True
            anim.begin()
        user32.SetTimer(self.hwnd, TIMER_ANIM, ANIM_MS, None)

    def _stop_anim(self):
        if self._anim_holding:
            self._anim_holding = False
            anim.end()
        if self.hwnd:
            user32.KillTimer(self.hwnd, TIMER_ANIM)

    def _anim_state(self):
        """Return (y_offset, constant_alpha) for the current animation frame."""
        dur = self._anim_dur
        if dur <= 0:
            return 0, 255
        u = (time.monotonic() - self._anim_t0) / dur
        if u >= 1.0:
            return (0, 255) if self._anim_in else (ANIM_DY_OUT, 0)
        u = max(0.0, u)
        e = 1.0 - (1.0 - u) ** 3          # ease-out cubic
        if self._anim_in:
            return int(round((1.0 - e) * ANIM_DY_IN)), int(round(255 * e))
        return int(round(e * ANIM_DY_OUT)), int(round(255 * (1.0 - e)))

    def _anim_tick(self):
        if not self.hwnd:
            return
        if self._exp_anim:
            if (time.monotonic() - self._exp_t0) >= self._exp_dur:
                self._exp_anim = False
                w, h = self._size[0], self._exp_h1
                self._size = (w, h)
                # re-render at the final height: the backdrop and the bitmap
                # must match the window rect 1:1
                self._refresh_backdrop(self._origin[0], self._origin[1], w, h)
                self._rebuild_surface()
                self._paint()
                if self._anim_dur <= 0:
                    self._stop_anim()
            else:
                self._paint()
            return
        if self._anim_dur <= 0:
            self._stop_anim()
            return
        done = (time.monotonic() - self._anim_t0) >= self._anim_dur
        self._paint()
        if done:
            self._stop_anim()
            self._anim_dur = 0.0
            if not self._open:
                self._destroy()

    def _destroy(self):
        if not self.hwnd:
            return
        hwnd = self.hwnd
        self.hwnd = 0
        if self._anim_holding:
            self._anim_holding = False
            anim.end()
        user32.KillTimer(hwnd, TIMER_ANIM)
        user32.KillTimer(hwnd, TIMER_WATCH)
        _hwnd_map.pop(hwnd, None)
        user32.DestroyWindow(hwnd)

    def _render_hover(self, idx):
        row = next((r for r in self.hits if r[0] == idx), None)
        if row is None:
            return None
        _, x0, y0, x1, y1 = row
        pal = _palette(self.dark, self.settings)
        img = self._base_img.copy()
        patch = _aa_patch(x1 - x0 + 1 - 2 * HOVER_INSET,
                          ITEM_H, ITEM_RADIUS, pal["hover"])
        img.alpha_composite(patch, (x0 + HOVER_INSET, y0))
        return to_bgra(img)

    def _hit(self, mx, my):
        for idx, x0, y0, x1, y1 in self.hits:
            if x0 <= mx <= x1 and y0 <= my <= y1:
                return idx
        return -1

    def _hit_control(self, mx, my):
        for kind, key, (x0, y0, x1, y1), payload in self.controls:
            if x0 <= mx <= x1 and y0 <= my <= y1:
                return kind, key, payload, (x0, x1)
        return None

    def _inside(self, mx, my):
        return 0 <= mx < self._size[0] and 0 <= my < self._size[1]

    def _dismiss(self):
        self.close()

    def close(self, animate: bool = True):
        global _menu_ref
        if not self.hwnd or not self._open:
            return
        _menu_ref = None
        self._open = False
        self._drag_slider = None
        self.hover = -1
        user32.KillTimer(self.hwnd, TIMER_WATCH)
        user32.ReleaseCapture()
        if animate and self._base_bgra is not None:
            # fade out instead of vanishing
            self._anim_in = False
            self._anim_t0 = time.monotonic()
            self._anim_dur = ANIM_OUT
            self._start_anim()
            self._paint()
        else:
            self._destroy()

    @property
    def is_open(self):
        return self._open

    # ---- outside-click watch ----
    def _watch(self):
        if not self._open:
            return
        lb = user32.GetAsyncKeyState(0x01) & 0x8000
        rb = user32.GetAsyncKeyState(0x02) & 0x8000

        if self._drag_slider is not None:
            if not lb:
                self._drag_slider = None
            return

        if time.monotonic() - self._opened_at < 0.15:
            return
        pt = POINT(0, 0)
        user32.GetCursorPos(ctypes.byref(pt))
        ox, oy = self._origin
        w, h = self._size
        if ox <= pt.x < ox + w and oy <= pt.y < oy + h:
            return
        if lb or rb:
            self.close()

    # ---- expanding ----
    def _set_expanded(self, expanded):
        if expanded == self.expanded:
            return
        w, h_exp = menu_size(self.items, True, header=self.header)
        _w, h_col = menu_size(self.items, False, header=self.header)
        h_target = h_exp if expanded else h_col

        x, y = self._origin
        wl, wt, wr, wb = screens.work_area_at(x, y)
        if y + h_target > wb - 8:
            y = max(wt + 8, wb - h_target - 8)
        self._origin = (x, y)
        self.expanded = expanded

        # While the reveal runs the surface is always the *expanded* layout and
        # the window keeps the expanded height — the strip is just cropped, so
        # the bitmap never has to be resized (UpdateLayeredWindow would scale
        # it, which is what made the backdrop look squashed).  Collapsing
        # animates the same strip from full height down to the collapsed one.
        self._size = (w, h_exp)
        self._refresh_backdrop(x, y, w, h_exp)
        self._rebuild_surface(expanded=True, chevron_t=None)
        self._exp_h0 = h_col if expanded else h_exp
        self._exp_h1 = h_target
        self._exp_t0 = time.monotonic()
        self._exp_anim = True
        if self.hwnd:
            self._start_anim()
            self._paint(move_to=(x, y))

    # ---- slider ----
    def _apply_slider(self, key, x, payload):
        vmin, vmax = payload
        tx0, tx1 = _slider_geometry()
        frac = 0.0 if tx1 == tx0 else (x - tx0) / (tx1 - tx0)
        frac = max(0.0, min(1.0, frac))
        val = int(round(vmin + frac * (vmax - vmin)))
        if self.settings.get(key) == val:
            return
        self.settings[key] = val
        if key == "blur":
            # the panel's own material uses the new radius too
            self.blur_radius = float(val)
            self._refresh_backdrop(*self._origin, *self._size)
        self._rebuild_surface()
        self._paint()
        if self.on_setting:
            try:
                self.on_setting(key, val)
            except Exception:
                pass

    # ---- messages ----
    def _handle(self, hwnd, msg, w, l):
        if msg == 0x0113:  # WM_TIMER
            if w == TIMER_WATCH:
                self._watch()
            elif w == TIMER_ANIM:
                self._anim_tick()
            return 0

        # while fading out the panel is inert
        if not self._open and msg in (0x0200, 0x0201, 0x0202, 0x0204, 0x0215):
            return 0

        if msg == 0x0200:  # WM_MOUSEMOVE
            x = ctypes.c_short(l & 0xFFFF).value
            y = ctypes.c_short((l >> 16) & 0xFFFF).value
            if self._drag_slider is not None:
                ctrl = next((c for c in self.controls if c[1] == self._drag_slider), None)
                if ctrl:
                    self._apply_slider(ctrl[1], x, ctrl[3])
                return 0
            inside = self._inside(x, y)
            new = self._hit(x, y) if inside else -1
            if new != self.hover:
                self.hover = new
                self._paint()
            return 0

        if msg == 0x0201:  # WM_LBUTTONDOWN
            x = ctypes.c_short(l & 0xFFFF).value
            y = ctypes.c_short((l >> 16) & 0xFFFF).value
            if not self._inside(x, y):
                self.close()
                return 0
            ctrl = self._hit_control(x, y)
            if ctrl is not None:
                kind, key, payload, _ = ctrl
                if kind == "slider":
                    self._drag_slider = key
                    self._apply_slider(key, x, payload)
                elif kind == "seg":
                    # `key` is "theme" or "size" — using the literal here made
                    # every segmented row write the theme.
                    if self.settings.get(key) != payload:
                        self.settings[key] = payload
                        self._rebuild_surface()
                        self._paint()
                        if self.on_setting:
                            try:
                                self.on_setting(key, payload)
                            except Exception:
                                pass
                return 0
            return 0

        if msg == 0x0202:  # WM_LBUTTONUP
            if self._drag_slider is not None:
                self._drag_slider = None
                return 0
            x = ctypes.c_short(l & 0xFFFF).value
            y = ctypes.c_short((l >> 16) & 0xFFFF).value
            if not self._inside(x, y):
                return 0
            idx = self._hit(x, y)
            if idx >= 0:
                it = self.items[idx]
                if it.expandable:
                    self._set_expanded(not self.expanded)
                    return 0
                if it.inline_toggle and not it.disabled:
                    # flip the checkmark in place — the menu stays open
                    it.checked = not it.checked
                    it.icon = "check" if it.checked else "checkbox_off"
                    try:
                        self.on_select(it.cmd)
                    except Exception:
                        pass
                    self._rebuild_surface()
                    self._paint()
                    return 0
                if not it.separator and not it.disabled and it.cmd:
                    try:
                        self.on_select(it.cmd)
                    except Exception:
                        pass
                    self.close()
            return 0

        if msg == 0x0204:  # WM_RBUTTONDOWN
            x = ctypes.c_short(l & 0xFFFF).value
            y = ctypes.c_short((l >> 16) & 0xFFFF).value
            if not self._inside(x, y):
                self.close()
            return 0

        if msg == 0x0215:  # WM_CAPTURECHANGED — re-acquire, never close
            if self._open and self.hwnd and not self._fixing_capture:
                self._fixing_capture = True
                try:
                    user32.SetCapture(self.hwnd)
                finally:
                    self._fixing_capture = False
            return 0

        if msg == 0x0010:  # WM_CLOSE
            self.close()
            return 0

        if msg == 0x0002:  # WM_DESTROY
            self._open = False
            return 0

        if msg in (0x0100, 0x0102):
            if w == 27:
                self.close()
                return 0
            return 0

        return user32.DefWindowProcW(hwnd, msg, w, l)


def warmup() -> None:
    """Pre-build masks, fonts and icon sprites so the first popup is instant.

    Runs off the UI thread at startup; creates no windows.
    """
    try:
        for empty in (False, True):
            items = default_items(True, empty)
            for expanded in (False, True):
                w, h = menu_size(items, expanded, header=True)
                _aa_mask(w, h, RADIUS)
                _border_layer(w, h, RADIUS, (255, 255, 255), 34)
                dummy = Image.new("RGB", (w, h), (48, 52, 74))
                for dark in (True, False):
                    render_menu(items, dark, 3, "status", None,
                                expanded=expanded,
                                settings={"opacity": 90, "material": 45,
                                          "blur": 24, "theme": "system"},
                                backdrop=dummy, header=True)
    except Exception:
        pass
