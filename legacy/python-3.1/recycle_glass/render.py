"""Fluent glass card renderer.

The card is a single RGBA surface composited in this order:

    drop shadow  ->  blurred wallpaper  ->  tint  ->  top sheen  ->  rim
                 ->  app icon  ->  text / action pills  ->  drop ripple

Everything is drawn at ``SCALE`` and every expensive sprite (masks, shadow,
gradients, resized icon) is cached, so an idle repaint stays under a
millisecond and a 125 Hz hover animation has room to breathe.
"""
from __future__ import annotations

import winreg
import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

from . import i18n
from . import squircle

# ---- fonts -------------------------------------------------------------
# Microsoft YaHei UI (index 1) is the shell's CJK UI face; Segoe UI Variable
# is the Win11 Latin face.  The bold faces are used for the title so the
# hierarchy reads without needing a larger size.
FONT_REGULAR = [
    ("C:/Windows/Fonts/msyh.ttc", 1),
    ("C:/Windows/Fonts/msyh.ttc", 0),
    ("C:/Windows/Fonts/segoeui.ttf", 0),
]
FONT_BOLD = [
    ("C:/Windows/Fonts/msyhbd.ttc", 1),
    ("C:/Windows/Fonts/msyhbd.ttc", 0),
    ("C:/Windows/Fonts/segoeuib.ttf", 0),
]
FONT_LATIN_REGULAR = [
    ("C:/Windows/Fonts/SegUIVar.ttf", 0),
    ("C:/Windows/Fonts/segoeui.ttf", 0),
]
FONT_LATIN_BOLD = [
    ("C:/Windows/Fonts/SegUIVar.ttf", 0),
    ("C:/Windows/Fonts/segoeuib.ttf", 0),
]

# ---- layout ------------------------------------------------------------
# Everything below is derived from BASE_* times SCALE, so one number drives
# the size presets and the optional DPI matching.
BASE = {
    "PAD": 34,            # transparent margin: room for the drop shadow
    "CARD_W": 302, "CARD_H": 118, "RADIUS": 28,
    "TITLE_FS": 22, "SUB_FS": 16,
    "ICON": 64, "ICON_LEFT": 18,
    "PILL_H": 32, "PILL_GAP": 8, "PILL_R": 10, "PILL_MAX_W": 96,
    "SHADOW_BLUR": 12, "SHADOW_DY": 6,
}

SCALE = 1.0

PAD = CARD_W = CARD_H = RADIUS = 0
TITLE_FS = SUB_FS = 0
ICON = ICON_X = ICON_Y = 0
PILL_H = PILL_GAP = PILL_R = PILL_MAX_W = 0
SHADOW_BLUR = SHADOW_DY = SHADOW_PAD = 0
WINDOW_W = WINDOW_H = 0


def set_scale(scale: float) -> None:
    """Rebuild every layout constant for the given scale factor."""
    global SCALE, PAD, CARD_W, CARD_H, RADIUS, TITLE_FS, SUB_FS
    global ICON, ICON_X, ICON_Y, PILL_H, PILL_GAP, PILL_R, PILL_MAX_W
    global SHADOW_BLUR, SHADOW_DY, SHADOW_PAD, WINDOW_W, WINDOW_H

    s = max(0.6, min(2.5, float(scale)))
    SCALE = s

    def k(name: str, lo: int) -> int:
        return max(lo, int(round(BASE[name] * s)))

    PAD = k("PAD", 16)
    CARD_W = k("CARD_W", 200)
    CARD_H = k("CARD_H", 78)
    RADIUS = k("RADIUS", 10)
    TITLE_FS = k("TITLE_FS", 11)
    SUB_FS = k("SUB_FS", 9)
    ICON = k("ICON", 34)
    ICON_X = PAD + k("ICON_LEFT", 10)
    ICON_Y = PAD + (CARD_H - ICON) // 2
    PILL_H = k("PILL_H", 18)
    PILL_GAP = k("PILL_GAP", 4)
    PILL_R = k("PILL_R", 5)
    PILL_MAX_W = k("PILL_MAX_W", 52)
    SHADOW_BLUR = k("SHADOW_BLUR", 4)
    SHADOW_DY = k("SHADOW_DY", 2)
    SHADOW_PAD = SHADOW_BLUR * 2 + 4
    WINDOW_W = PAD * 2 + CARD_W
    WINDOW_H = PAD * 2 + CARD_H


set_scale(1.0)

ASSET_DIR = Path(__file__).resolve().parent / "assets"

_FONT_CACHE: dict[tuple, ImageFont.FreeTypeFont | ImageFont.ImageFont] = {}


def _pick(candidates) -> tuple[str, int] | None:
    for path, index in candidates:
        if Path(path).exists():
            return path, index
    return None


def _font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    key = (size, bold)
    f = _FONT_CACHE.get(key)
    if f is not None:
        return f
    latin = not i18n.is_cjk()
    cands = (FONT_LATIN_BOLD if latin else FONT_BOLD) if bold else (
        FONT_LATIN_REGULAR if latin else FONT_REGULAR)
    chosen = _pick(cands) or _pick(FONT_REGULAR)
    f = None
    if chosen:
        try:
            f = ImageFont.truetype(chosen[0], size, index=chosen[1])
        except OSError:
            f = None
    if f is None:
        f = ImageFont.load_default()
    _FONT_CACHE[key] = f
    return f


_LIGHT_CACHE = {"at": -1e9, "value": True}


def system_is_light(ttl: float = 2.0) -> bool:
    """AppsUseLightTheme with a short cache — it is read on every repaint."""
    import time
    now = time.monotonic()
    if now - _LIGHT_CACHE["at"] < ttl:
        return bool(_LIGHT_CACHE["value"])
    val = _system_is_light_uncached()
    _LIGHT_CACHE["at"] = now
    _LIGHT_CACHE["value"] = val
    return val


def _system_is_light_uncached() -> bool:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
        ) as k:
            val, _ = winreg.QueryValueEx(k, "AppsUseLightTheme")
            return bool(val)
    except OSError:
        return True


@dataclass
class Palette:
    tint: tuple[int, int, int]
    text: tuple[int, int, int]
    text_dim: tuple[int, int, int]
    border: tuple[int, int, int, int]
    pill_bg: tuple[int, int, int, int]
    pill_text: tuple[int, int, int]
    chip_bg: tuple[int, int, int, int]
    chip_border: tuple[int, int, int, int]
    pill_hover_bg: tuple[int, int, int, int] = (255, 255, 255, 40)
    pill_hover_border: tuple[int, int, int, int] = (255, 255, 255, 110)
    drag_wash: tuple[int, int, int, int] = (126, 186, 252, 34)
    drag_ring: tuple[int, int, int, int] = (126, 186, 252, 205)
    icon_shadow: tuple[int, float] | None = None
    rim: tuple[int, int, int] | None = (2, 96, 34)
    tint_floor: float = 0.0
    tint_span: float = 1.0
    opacity_floor: float = 0.0
    opacity_span: float = 1.0
    # new: shadow strength, sheen strength, chip gradient
    shadow_alpha: int = 110
    sheen_top: int = 26
    sheen_bottom: int = 0
    bottom_shade: int = 0
    chip_top: tuple[int, int, int, int] = (255, 255, 255, 16)
    chip_bottom: tuple[int, int, int, int] = (255, 255, 255, 4)
    accent: tuple[int, int, int] = (126, 186, 252)
    # a 1 px dark halo keeps light text readable when the wallpaper behind the
    # glass happens to be bright (material = 0 means almost no tint at all)
    text_shadow: tuple[int, int, int] | None = None


def palette_for(theme: str) -> Palette:
    light = theme == "light" or (theme == "system" and system_is_light())
    if light:
        # Cool near-white glass: the wallpaper still shows through, but the
        # surface stays bright enough for dark text on any wallpaper.
        return Palette(
            tint=(252, 252, 255),
            text=(26, 27, 31),
            text_dim=(64, 66, 78),
            border=(0, 0, 0, 26),
            pill_bg=(255, 255, 255, 238),
            pill_text=(26, 27, 31),
            chip_bg=(0, 0, 0, 12),
            chip_border=(0, 0, 0, 16),
            pill_hover_bg=(0, 0, 0, 12),
            pill_hover_border=(0, 122, 255, 200),
            drag_wash=(0, 122, 255, 34),
            drag_ring=(0, 122, 255, 220),
            icon_shadow=(64, 6.0),
            rim=(2, 205, 78),
            tint_floor=0.50,
            tint_span=0.42,
            opacity_floor=0.74,
            opacity_span=0.26,
            shadow_alpha=48,
            sheen_top=118,
            sheen_bottom=6,
            bottom_shade=10,
            chip_top=(255, 255, 255, 150),
            chip_bottom=(255, 255, 255, 60),
            accent=(0, 122, 255),
        )
    return Palette(
        tint=(34, 36, 46),
        text=(246, 247, 250),
        text_dim=(198, 202, 214),
        border=(255, 255, 255, 26),
        pill_bg=(255, 255, 255, 26),
        pill_text=(246, 247, 250),
        chip_bg=(255, 255, 255, 16),
        chip_border=(255, 255, 255, 20),
        pill_hover_bg=(255, 255, 255, 52),
        pill_hover_border=(255, 255, 255, 130),
        drag_wash=(126, 186, 252, 40),
        drag_ring=(126, 186, 252, 215),
        icon_shadow=(110, 7.0),
        rim=(2, 195, 62),
        tint_floor=0.18,
        tint_span=0.74,
        opacity_floor=0.0,
        opacity_span=1.0,
        shadow_alpha=130,
        sheen_top=44,
        sheen_bottom=0,
        bottom_shade=26,
        chip_top=(255, 255, 255, 22),
        chip_bottom=(255, 255, 255, 6),
        accent=(126, 186, 252),
        text_shadow=(0, 0, 0),
    )


_APP_ICON_CACHE: dict[str, Image.Image] = {}


def load_app_icon() -> Image.Image | None:
    """Load the 1920 px app icon once (decoding a large PNG is expensive)."""
    p = ASSET_DIR / "app-icon-1920.png"
    key = str(p)
    got = _APP_ICON_CACHE.get(key)
    if got is not None:
        return got
    if p.exists():
        try:
            img = Image.open(p).convert("RGBA")
        except OSError:
            return None
        _APP_ICON_CACHE[key] = img
        return img
    return None


_SS = 6  # supersampling factor — ImageDraw has no anti-aliasing

_MASK_CACHE: dict[tuple, Image.Image] = {}
_SPRITE_CACHE: dict[tuple, Image.Image] = {}


def _round_rect_mask(size: tuple[int, int], box: tuple[int, int, int, int],
                     radius: int) -> Image.Image:
    """Anti-aliased superellipse-cornered coverage mask (0..255)."""
    w, h = size
    ss = _SS
    big = Image.new("L", (w * ss, h * ss), 0)
    bw = (box[2] - box[0] + 1) * ss
    bh = (box[3] - box[1] + 1) * ss
    pts = squircle.curve(bw, bh, radius * ss, (box[0] * ss, box[1] * ss))
    ImageDraw.Draw(big).polygon(pts, fill=255)
    return big.resize((w, h), Image.Resampling.LANCZOS)


def _card_mask(card_w: int, card_h: int, radius: int) -> Image.Image:
    key = ("m", card_w, card_h, radius)
    m = _MASK_CACHE.get(key)
    if m is None:
        m = _round_rect_mask((card_w, card_h), (0, 0, card_w - 1, card_h - 1), radius)
        _MASK_CACHE[key] = m
    return m


def _ring_mask(w: int, h: int, radius: int, thickness: int = 1) -> Image.Image:
    """Hairline coverage just inside the edge (AA)."""
    key = ("rm", w, h, radius, thickness)
    got = _MASK_CACHE.get(key)
    if got is None:
        t = max(1, thickness)
        outer = _round_rect_mask((w, h), (0, 0, w - 1, h - 1), radius)
        inner = _round_rect_mask((w, h), (t, t, w - 1 - t, h - 1 - t),
                                 max(0, radius - t))
        got = ImageChops.subtract(outer, inner)
        _MASK_CACHE[key] = got
    return got


def _rim_layer(w: int, h: int, radius: int, top_a: int, bottom_a: int,
               rgb=(255, 255, 255), glow: float = 0.55) -> Image.Image:
    """Glass rim: bright along the top, softer down the sides, plus an inner glow.

    Measured off a reference glass panel: the top edge is ~+80 luma brighter
    than the surface, the sides ~+30, and there is a 3-6 px halo just inside
    the edge.  A plain 2 px hairline reads flat next to that, so the rim is a
    1 px line plus two wider rings at lower alpha, all scaled by a vertical
    ramp that decays quickly away from the top.
    """
    key = ("rim", w, h, radius, top_a, bottom_a, tuple(rgb), round(glow, 2))
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got

    strip = Image.new("L", (1, h))
    for y in range(h):
        u = y / max(1, h - 1)
        a = bottom_a + (top_a - bottom_a) * math.exp(-5.5 * u)
        strip.putpixel((0, y), max(0, min(255, int(round(a)))))
    ramp = strip.resize((w, h), Image.Resampling.BILINEAR)

    # A blurred hairline gives a *continuous* falloff.  Three discrete rings
    # (1/3/6 px) left visible steps — the "色阶" the user could see.
    ring = _ring_mask(w, h, radius, 1)
    halo = ring.filter(ImageFilter.GaussianBlur(max(1.2, radius * 0.13)))
    halo = ImageChops.multiply(halo, _card_mask(w, h, radius))
    cover = ImageChops.add(ring, halo.point(lambda v: int(v * glow)))

    layer = Image.new("RGBA", (w, h), (*rgb, 0))
    layer.putalpha(ImageChops.multiply(cover, ramp))
    _SPRITE_CACHE[key] = layer
    return layer


def _ring_layer(w: int, h: int, radius: int, color, scale: float = 1.0,
                thickness: int = 1) -> Image.Image:
    """AA hairline ring in `color` (RGBA); `scale` multiplies its alpha."""
    rgb = tuple(color[:3])
    base_a = color[3] if len(color) > 3 else 255
    q = round(round(scale * 20) / 20, 2)   # quantise so the cache stays small
    key = ("rl", w, h, radius, rgb, base_a, q, thickness)
    got = _MASK_CACHE.get(key)
    if got is not None:
        return got
    ring = _ring_mask(w, h, radius, thickness)
    a = ring.point(lambda v: int(v * base_a * q / 255))
    layer = Image.new("RGBA", (w, h), (*rgb, 0))
    layer.putalpha(a)
    _MASK_CACHE[key] = layer
    return layer


def _aa_patch(w: int, h: int, radius: int, color) -> Image.Image:
    """Anti-aliased rounded rectangle in `color` (RGBA)."""
    key = ("p", w, h, radius, tuple(color))
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got
    big = Image.new("RGBA", (w * _SS, h * _SS), (0, 0, 0, 0))
    pts = squircle.polygon(w * _SS, h * _SS, radius * _SS)
    ImageDraw.Draw(big).polygon(pts, fill=color)
    got = big.resize((w, h), Image.Resampling.LANCZOS)
    _SPRITE_CACHE[key] = got
    return got


def _vgrad(w: int, h: int, top, bottom) -> Image.Image:
    """Vertical linear gradient (RGBA), cached."""
    key = ("vg", w, h, tuple(top), tuple(bottom))
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got
    strip = Image.new("RGBA", (1, h))
    for y in range(h):
        u = y / max(1, h - 1)
        strip.putpixel((0, y), tuple(
            int(round(top[i] + (bottom[i] - top[i]) * u)) for i in range(4)))
    got = strip.resize((w, h), Image.Resampling.BILINEAR)
    _SPRITE_CACHE[key] = got
    return got


def _shadow_sprite(w: int, h: int, radius: int, blur: int, dy: int,
                   alpha: int) -> Image.Image:
    """Soft drop shadow for the card, padded by SHADOW_PAD on every side."""
    key = ("sh", w, h, radius, blur, alpha)
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got
    pad = SHADOW_PAD
    sw, sh = w + pad * 2, h + pad * 2
    mask = Image.new("L", (sw, sh), 0)
    pts = squircle.curve(w, h, radius, (pad, pad))
    ImageDraw.Draw(mask).polygon(pts, fill=max(0, min(255, alpha)))
    if blur > 0:
        mask = mask.filter(ImageFilter.GaussianBlur(blur))
    spr = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    spr.putalpha(mask)
    _SPRITE_CACHE[key] = spr
    return spr


def _sheen(w: int, h: int, top_a: int, mid_a: int, shade_a: int) -> Image.Image:
    """Top-down light falloff + a soft bottom shade — the 'lit glass' read.

    Both curves are smooth over the full height: a piecewise-linear version
    left a visible band where the gradient changed slope, which read as a
    seam across the middle of the card.
    """
    key = ("sheen", w, h, top_a, mid_a, shade_a)
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got
    # Two independent gradients, composited.  Doing it with a single
    # `if white ... elif dark` pass put a hard step where the white curve hit
    # zero while the dark curve had already reached ~17/255 — that step was
    # the horizontal line visible across the bottom of the card.
    light = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pl, ps = light.load(), shade.load()
    for y in range(h):
        u = y / max(1, h - 1)
        wa = int(round(top_a * (1.0 - u) ** 2.2 + mid_a * (1.0 - u) ** 6))
        da = int(round(shade_a * (u ** 3.2)))
        if wa > 0:
            col_l = (255, 255, 255, min(255, wa))
            for x in range(w):
                pl[x, y] = col_l
        if da > 0:
            col_s = (0, 0, 0, min(255, da))
            for x in range(w):
                ps[x, y] = col_s
    layer = Image.alpha_composite(light, shade)
    _SPRITE_CACHE[key] = layer
    return layer


_ICON_CACHE: dict[tuple, Image.Image] = {}


def _icon_base(icon: Image.Image) -> Image.Image:
    """One high-quality downscale of the source, reused afterwards."""
    key = ("ibase", id(icon))
    got = _ICON_CACHE.get(key)
    if got is None:
        b = max(160, ICON * 2)
        got = icon.resize((b, b), Image.Resampling.LANCZOS)
        _ICON_CACHE[key] = got
    return got


def _icon_at(icon: Image.Image, size: int) -> Image.Image:
    key = ("isz", id(icon), size)
    got = _ICON_CACHE.get(key)
    if got is None:
        base = _icon_base(icon)
        got = (base if size == base.width
               else base.resize((size, size), Image.Resampling.LANCZOS))
        _ICON_CACHE[key] = got
    return got


def _icon_shadow(size: int, alpha: int, blur: float) -> tuple[Image.Image, int]:
    """Soft drop shadow sprite for the icon tile, returned with its padding."""
    key = ("ish", size, alpha, blur)
    got = _ICON_CACHE.get(key)
    if got is None:
        pad = 24
        d = size + pad
        mask = Image.new("L", (d, d), 0)
        pts = squircle.curve(size, size, max(2, int(size * 0.24)),
                             (pad // 2, pad // 2 + 2))
        ImageDraw.Draw(mask).polygon(pts, fill=alpha)
        mask = mask.filter(ImageFilter.GaussianBlur(blur))
        spr = Image.new("RGBA", (d, d), (0, 0, 0, 0))
        spr.putalpha(mask)
        got = spr
        _ICON_CACHE[key] = got
    return got, 12


def _aa_ring(diameter: int, thickness: int, color) -> Image.Image:
    """Anti-aliased circle outline, used for the drop ripple."""
    key = ("circ", diameter, thickness, tuple(color))
    got = _SPRITE_CACHE.get(key)
    if got is not None:
        return got
    d = max(4, diameter)
    big = Image.new("RGBA", (d * _SS, d * _SS), (0, 0, 0, 0))
    ImageDraw.Draw(big).ellipse((0, 0, d * _SS - 1, d * _SS - 1),
                                outline=color, width=max(1, thickness * _SS))
    got = big.resize((d, d), Image.Resampling.LANCZOS)
    _SPRITE_CACHE[key] = got
    return got


def _aa_ring_alpha(diameter: int, thickness: int, rgb, alpha: int) -> Image.Image:
    """Ring sprite with its alpha applied, cached per quantised alpha."""
    base = _aa_ring(diameter, thickness, (*rgb, 255))
    if alpha >= 255:
        return base
    q = max(0, min(255, (alpha // 8) * 8))
    key = ("ringa", diameter, thickness, tuple(rgb), q)
    got = _SPRITE_CACHE.get(key)
    if got is None:
        got = base.copy()
        got.putalpha(base.getchannel("A").point(lambda v: v * q // 255))
        _SPRITE_CACHE[key] = got
    return got


def _scaled_alpha(layer: Image.Image, a: float) -> Image.Image:
    """Multiply a layer's alpha (used for the hover cross-fade)."""
    a = max(0.0, min(1.0, a))
    if a >= 0.999:
        return layer
    out = layer.copy()
    out.putalpha(layer.getchannel("A").point(lambda v: int(v * a)))
    return out


def _ease_out(u: float) -> float:
    u = max(0.0, min(1.0, u))
    return 1.0 - (1.0 - u) ** 3


def render(
    window_size: tuple[int, int],
    backdrop: Image.Image | None,
    icon: Image.Image | None,
    title: str,
    subtitle: str,
    hover: bool,
    palette: Palette,
    opacity: float,
    material: float,
    hover_buttons: tuple[str, str] = ("打开", "清空"),
    hover_btn: str | None = None,
    drag_hover: bool = False,
    fx_t: float | None = None,
    native: bool = False,
    hover_t: float = 1.0,
) -> tuple[Image.Image, list[tuple[str, int, int, int, int]]]:
    """Return (RGBA surface, hit-rects [(label, x0, y0, x1, y1)]).

    ``hover_t`` animates the second row: 0 shows the size/count line, 1 shows
    the action pills, and everything in between cross-fades (the caller drives
    it from a ~120 Hz timer).
    """
    W, H = window_size
    card_w, card_h = CARD_W, CARD_H
    ox, oy = PAD, PAD
    hover_t = _ease_out(max(0.0, min(1.0, hover_t)))
    if not hover:
        hover_t = 0.0

    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # ---- 1. drop shadow (grows slightly while hovered) ------------------
    # quantise the hover progress: each distinct alpha costs one blurred
    # sprite (~8 ms), so 5 levels instead of one per animation frame
    q_t = round(hover_t * 4) / 4.0
    sh_a = int(palette.shadow_alpha * (1.0 + 0.35 * q_t))
    sh = _shadow_sprite(card_w, card_h, RADIUS, SHADOW_BLUR, SHADOW_DY,
                        sh_a)
    canvas.alpha_composite(sh, (ox - SHADOW_PAD, oy - SHADOW_PAD + SHADOW_DY))

    # ---- 2. glass body --------------------------------------------------
    if native or backdrop is None:
        body = Image.new("RGBA", (card_w, card_h), (*palette.tint, 255))
        a = 1.0
    else:
        body = backdrop.convert("RGBA")
        if body.size != (card_w, card_h):
            body = body.resize((card_w, card_h), Image.Resampling.BILINEAR)
        a = max(0.0, min(1.0, palette.tint_floor + palette.tint_span * material))
        body = Image.blend(body, Image.new("RGBA", body.size,
                                           (*palette.tint, 255)), a)

    # lit-from-above sheen: the single biggest cue that this is glass and not
    # a flat grey panel
    body.alpha_composite(_sheen(card_w, card_h, palette.sheen_top,
                               max(0, palette.sheen_top // 3),
                               palette.bottom_shade))

    fac = max(0.15, min(1.0,
                        palette.opacity_floor + palette.opacity_span * opacity))
    mask = _card_mask(card_w, card_h, RADIUS)
    if native:
        tint_a = palette.tint_floor + palette.tint_span * material
        fac = max(0.10, min(1.0, fac * tint_a))
    if fac < 0.999:
        mask = mask.point(lambda v: int(v * fac))
    body.putalpha(mask)
    canvas.alpha_composite(body, (ox, oy))

    # ---- 3. rim: soft top highlight + hairline edge ---------------------
    if not native:
        if palette.rim is not None:
            th, ta, ba = palette.rim
            canvas.alpha_composite(
                _rim_layer(card_w, card_h, RADIUS,
                           int(ta * fac), int(ba * fac)),
                (ox, oy))
        canvas.alpha_composite(
            _ring_layer(card_w, card_h, RADIUS, palette.border, fac),
            (ox, oy))

    if drag_hover:
        wash = Image.new("RGBA", (card_w, card_h),
                         (*palette.drag_wash[:3], 255))
        wa = max(0.0, min(1.0, opacity))
        wash.putalpha(mask.point(
            lambda v: int(v * palette.drag_wash[3] * wa / 255)))
        canvas.alpha_composite(wash, (ox, oy))
        canvas.alpha_composite(
            _ring_layer(card_w, card_h, RADIUS, palette.drag_ring, wa, 2),
            (ox, oy))

    # ---- 4. icon --------------------------------------------------------
    scale = 1.0
    if fx_t is not None:
        if fx_t < 0.35:
            scale = 1.0 - 0.16 * (fx_t / 0.35)
        elif fx_t < 0.70:
            scale = 0.84 + 0.24 * ((fx_t - 0.35) / 0.35)
        else:
            scale = 1.08 - 0.08 * ((fx_t - 0.70) / 0.30)

    if icon is not None:
        # the app icon is already a rounded tile — give it air, not a frame
        if palette.icon_shadow is not None:
            spr, pad = _icon_shadow(ICON, palette.icon_shadow[0],
                                    palette.icon_shadow[1])
            canvas.alpha_composite(spr, (ICON_X - pad, ICON_Y - pad))
        if abs(scale - 1.0) > 0.01:
            sz = max(8, int(round(ICON * scale)))
            canvas.alpha_composite(
                _icon_at(icon, sz),
                (ICON_X + (ICON - sz) // 2, ICON_Y + (ICON - sz) // 2))
        else:
            canvas.alpha_composite(_icon_at(icon, ICON), (ICON_X, ICON_Y))
    else:
        # fallback: the stock shell icon needs a container to look intentional
        chip_r = int(ICON * 0.26)
        canvas.alpha_composite(_aa_patch(ICON, ICON, chip_r, palette.chip_bg),
                               (ICON_X, ICON_Y))
        canvas.alpha_composite(
            _ring_layer(ICON, ICON, chip_r, palette.chip_border),
            (ICON_X, ICON_Y))

    # ---- 5. text --------------------------------------------------------
    title_font = _font(TITLE_FS, bold=True)
    sub_font = _font(SUB_FS)

    text_x = ICON_X + ICON + max(14, int(round(18 * SCALE)))
    text_right = PAD + CARD_W - max(12, int(round(20 * SCALE)))
    content_w = max(60, text_right - text_x)

    tb = title_font.getbbox(title)
    title_ink_h = tb[3] - tb[1]
    sb0 = sub_font.getbbox(subtitle)
    sub_ink_h = sb0[3] - sb0[1]
    # The idle block (title + subtitle) and the hover block (title + pills)
    # have different natural heights.  Interpolating them with hover_t keeps
    # the title/subtitle gap tight when idle instead of leaving the subtitle
    # floating at the bottom of a 32 px pill row.
    sub_gap = max(3, int(round(5 * SCALE)))
    row_gap = max(5, int(round(8 * SCALE)))
    idle_h = title_ink_h + sub_gap + sub_ink_h
    hover_h = title_ink_h + row_gap + PILL_H
    content_h = idle_h + (hover_h - idle_h) * hover_t
    block_top = PAD + int(round((CARD_H - content_h) / 2))
    title_y = block_top - tb[1]
    row_top = (block_top + title_ink_h
               + int(round(sub_gap + (row_gap - sub_gap) * hover_t)))

    txt = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    td = ImageDraw.Draw(txt)
    if palette.text_shadow is not None:
        td.text((text_x, title_y + max(1, int(round(SCALE)))),
                title, font=title_font,
                fill=(*palette.text_shadow, 110))
    td.text((text_x, title_y), title, font=title_font,
            fill=(*palette.text, 255))
    canvas.alpha_composite(txt)

    # ---- hit rects first, drawing second -------------------------------
    # The pills fade in over 160 ms.  Computing their geometry only while
    # they are visible meant a click during the fade hit nothing ("点了没反应").
    # Geometry is independent of the animation, so publish it immediately.
    hit_rects: list[tuple[str, int, int, int, int]] = []
    n_btn = len(hover_buttons)
    bw = min(PILL_MAX_W, (content_w - PILL_GAP * (n_btn - 1)) // n_btn)
    total_w = bw * n_btn + PILL_GAP * (n_btn - 1)
    bx0 = text_x + max(0, (content_w - total_w) // 2)
    if hover:
        bx = bx0
        for lab in hover_buttons:
            hit_rects.append((lab, bx, row_top, bx + bw - 1,
                              row_top + PILL_H - 1))
            bx += bw + PILL_GAP

    # second row: subtitle (idle) cross-fades into the action pills (hover).
    # The two states never fully overlap — the subtitle is gone before the
    # pills are solid, otherwise the text shows through the translucent pills.
    sub_a = max(0.0, min(1.0, 1.0 - hover_t / 0.45))
    pill_a = max(0.0, min(1.0, (hover_t - 0.32) / 0.68))
    if sub_a > 0.001:
        sb = sub_font.getbbox(subtitle)
        sub_y = row_top - sb[1]
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        if palette.text_shadow is not None:
            ld.text((text_x, sub_y + max(1, int(round(SCALE)))),
                    subtitle, font=sub_font,
                    fill=(*palette.text_shadow, 100))
        ld.text((text_x, sub_y), subtitle, font=sub_font,
                fill=(*palette.text_dim, 255))
        canvas.alpha_composite(_scaled_alpha(layer, sub_a))

    if pill_a > 0.001:
        dy = int(round((1.0 - pill_a) * max(3, int(6 * SCALE))))
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        bx = bx0
        for lab in hover_buttons:
            lit = (hover_btn == lab)
            layer.alpha_composite(
                _aa_patch(bw, PILL_H, PILL_R,
                          palette.pill_hover_bg if lit else palette.pill_bg),
                (bx, row_top + dy))
            if lit:
                layer.alpha_composite(
                    _ring_layer(bw, PILL_H, PILL_R, palette.pill_hover_border),
                    (bx, row_top + dy))
            d2 = ImageDraw.Draw(layer)
            tw = int(d2.textlength(lab, font=sub_font))
            sb2 = sub_font.getbbox(lab)
            ty = row_top + dy + (PILL_H - (sb2[3] - sb2[1])) // 2 - sb2[1]
            d2.text((bx + (bw - tw) // 2, ty), lab, font=sub_font,
                    fill=(*palette.pill_text, 255))
            bx += bw + PILL_GAP
        canvas.alpha_composite(_scaled_alpha(layer, pill_a))

    # ---- 6. drop ripple -------------------------------------------------
    if fx_t is not None:
        t = max(0.0, min(1.0, fx_t))
        cx = ICON_X + ICON // 2
        cy = ICON_Y + ICON // 2
        dia = int((28 + t * 84) * SCALE)
        alpha = int(215 * (1.0 - t) ** 1.4)
        if alpha > 4:
            th = max(1, int(round((3 - 2 * t) * SCALE)))
            ring = _aa_ring_alpha(dia, th, palette.drag_ring[:3], alpha)
            canvas.alpha_composite(ring, (cx - dia // 2, cy - dia // 2))

    return canvas, hit_rects


def to_bgra(img: Image.Image) -> bytes:
    """Convert RGBA PIL image to premultiplied BGRA (UpdateLayeredWindow format).

    Uses PIL's raw mode "BGRa" (premultiplied BGRA) for a single C-level
    conversion — dramatically faster than a Python pixel loop.
    """
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    try:
        return img.tobytes("raw", "BGRa")
    except Exception:
        pass
    w, h = img.size
    src = img.tobytes()
    out = bytearray(w * h * 4)
    for i in range(w * h):
        r, g, b, a = src[i * 4], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]
        out[i * 4] = (b * a) // 255
        out[i * 4 + 1] = (g * a) // 255
        out[i * 4 + 2] = (r * a) // 255
        out[i * 4 + 3] = a
    return bytes(out)
