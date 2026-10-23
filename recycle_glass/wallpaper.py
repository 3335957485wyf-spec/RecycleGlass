"""Capture the wallpaper underneath a screen rect and Gaussian-blur it.

Why we blur the wallpaper ourselves instead of asking DWM for a backdrop:
the requested look is a clean Gaussian blur with no acrylic grain, and a
per-pixel-alpha layered window cannot sample a DWM backdrop anyway.

Multi-monitor notes
-------------------
Windows paints each monitor separately unless the wallpaper is set to "span".
Placement is therefore computed against the *monitor* the card sits on, and
against the *virtual desktop* only in span mode.  The previous version always
used the primary screen size, so the glass showed the wrong part of the
picture (or a flat grey band) on every secondary display.
"""
from __future__ import annotations

import ctypes
from pathlib import Path

from PIL import Image, ImageFilter

from . import diag
from .w32 import user32

# wallpaper style codes under HKCU\Control Panel\Desktop
#   WallpaperStyle 0 + TileWallpaper 0  -> centred
#   WallpaperStyle 0 + TileWallpaper 1  -> tiled
#   WallpaperStyle 2                    -> stretched
#   WallpaperStyle 6                    -> fit
#   WallpaperStyle 10                   -> fill
#   WallpaperStyle 22                   -> span


def _reg_read(key: str, name: str) -> str | None:
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
            val, _ = winreg.QueryValueEx(k, name)
            return str(val)
    except OSError:
        return None


def wallpaper_path() -> str | None:
    buf = ctypes.create_unicode_buffer(1024)
    if user32.SystemParametersInfoW(0x0073, 1024, buf, 0):  # SPI_GETDESKWALLPAPER
        p = buf.value
        if p and Path(p).exists():
            return p
    p = _reg_read(r"Control Panel\Desktop", "WallPaper")
    if p and Path(p).exists():
        return p
    return None


def _desktop_color() -> tuple[int, int, int]:
    """Solid desktop background colour — the fallback when there is no image."""
    raw = _reg_read(r"Control Panel\Colors", "Background") or "0 0 0"
    try:
        parts = [int(x) for x in raw.split()[:3]]
        if len(parts) == 3:
            return tuple(max(0, min(255, v)) for v in parts)  # type: ignore[return-value]
    except (ValueError, TypeError):
        pass
    return (0, 0, 0)


def _wallpaper_style() -> tuple[int, bool]:
    style = _reg_read(r"Control Panel\Desktop", "WallpaperStyle") or "10"
    tile = _reg_read(r"Control Panel\Desktop", "TileWallpaper") or "0"
    try:
        return int(style), tile == "1"
    except ValueError:
        return 10, False


# ---------------------------------------------------------------- geometry

def _virtual_rect() -> tuple[int, int, int, int]:
    x = user32.GetSystemMetrics(76)
    y = user32.GetSystemMetrics(77)
    w = user32.GetSystemMetrics(78)
    h = user32.GetSystemMetrics(79)
    if w <= 0 or h <= 0:
        w, h = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
        x = y = 0
    return x, y, x + w, y + h


def _surface_for(rect: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    """The desktop surface this rect is painted on (monitor, or all of them)."""
    style, _tile = _wallpaper_style()
    if style == 22:
        return _virtual_rect()
    from . import screens
    l, t, r, b = rect
    return screens.monitor_rect_at((l + r) // 2, (t + b) // 2)


def _place(img_w: int, img_h: int, surf: tuple[int, int, int, int],
           style: int, tile: bool) -> tuple[int, int, int, int]:
    """Return (offset_x, offset_y, scaled_w, scaled_h) inside `surf`."""
    sl, st, sr, sb = surf
    sw, sh = max(1, sr - sl), max(1, sb - st)
    if style == 2:  # stretch
        return sl, st, sw, sh
    if style == 6:  # fit
        k = min(sw / img_w, sh / img_h)
        w, h = max(1, int(img_w * k)), max(1, int(img_h * k))
        return sl + (sw - w) // 2, st + (sh - h) // 2, w, h
    if style == 10:  # fill
        k = max(sw / img_w, sh / img_h)
        w, h = max(1, int(img_w * k)), max(1, int(img_h * k))
        return sl + (sw - w) // 2, st + (sh - h) // 2, w, h
    if style == 22:  # span
        k = max(sw / img_w, sh / img_h)
        w, h = max(1, int(img_w * k)), max(1, int(img_h * k))
        return sl + (sw - w) // 2, st + (sh - h) // 2, w, h
    if tile:
        return sl, st, img_w, img_h
    # centred
    return sl + (sw - img_w) // 2, st + (sh - img_h) // 2, img_w, img_h


# ---------------------------------------------------------------- caching

_img_cache: dict[str, object] = {"key": None, "img": None}
_scaled_cache: dict[object, tuple] = {}
_blur_screen: dict[object, tuple] = {}
_blur_order: list[object] = []


def _load_wallpaper(path: str) -> Image.Image | None:
    try:
        mtime = Path(path).stat().st_mtime
    except OSError:
        mtime = None
    key = f"{path}|{mtime}"
    if _img_cache["key"] == key:
        img = _img_cache["img"]
        return img if isinstance(img, Image.Image) else None
    try:
        img = Image.open(path).convert("RGB")
    except OSError as e:
        diag.warn("wallpaper unreadable: %s", e)
        return None
    _img_cache["key"] = key
    _img_cache["img"] = img
    return img


def _screen_space(img: Image.Image, surf: tuple[int, int, int, int]):
    """(scaled image, offset x, offset y) for this desktop surface."""
    style, tile = _wallpaper_style()
    key = (id(img), img.size, style, tile, surf)
    got = _scaled_cache.get(key)
    if got is not None:
        return got
    ox, oy, sw, sh = _place(img.width, img.height, surf, style, tile)
    if tile and style != 22:
        # repeat the bitmap across the surface (bounded: a 4K desktop tiled
        # with a tiny pattern would otherwise cost tens of thousands of pastes)
        sl, st, sr, sb = surf
        cw, ch = max(1, sr - sl), max(1, sb - st)
        nx = (cw + img.width - 1) // max(1, img.width)
        ny = (ch + img.height - 1) // max(1, img.height)
        if nx * ny <= 4096:
            canvas = Image.new("RGB", (cw, ch), _desktop_color())
            for yy in range(ny):
                for xx in range(nx):
                    canvas.paste(img, (xx * img.width, yy * img.height))
            ox, oy, sw, sh = sl, st, cw, ch
            scaled = canvas
        else:
            k = max(cw / img.width, ch / img.height)
            sw, sh = max(1, int(img.width * k)), max(1, int(img.height * k))
            ox, oy = sl + (cw - sw) // 2, st + (ch - sh) // 2
            scaled = img.resize((sw, sh), Image.Resampling.BILINEAR)
    elif (sw, sh) != img.size:
        # BILINEAR: invisible after the blur, far cheaper than LANCZOS
        scaled = img.resize((sw, sh), Image.Resampling.BILINEAR)
    else:
        scaled = img
    got = (scaled, ox, oy)
    if len(_scaled_cache) > 6:
        _scaled_cache.clear()
    _scaled_cache[key] = got
    return got


def _blurred_screen(radius: float, surf: tuple[int, int, int, int]):
    """(blurred surface, downsample factor, offset x, offset y) or None."""
    key = (round(float(radius), 1), surf)
    ent = _blur_screen.get(key)
    if ent is not None:
        return ent

    path = wallpaper_path()
    img = _load_wallpaper(path) if path else None
    if img is None:
        return None

    scaled, ox, oy = _screen_space(img, surf)
    if radius > 0:
        factor = 3
        out = scaled.resize(
            (max(1, scaled.width // factor), max(1, scaled.height // factor)),
            Image.Resampling.BILINEAR)
        out = out.filter(ImageFilter.GaussianBlur(max(0.5, float(radius) / factor)))
    else:
        factor = 1
        out = scaled

    ent = (out, factor, ox, oy)
    while len(_blur_order) >= 3:
        _blur_screen.pop(_blur_order.pop(0), None)
    _blur_screen[key] = ent
    _blur_order.append(key)
    return ent


# ---------------------------------------------------------------- public

def blurred_backdrop(
    rect: tuple[int, int, int, int],
    radius: float = 24.0,
) -> Image.Image | None:
    """Return an RGB image of the desktop behind `rect`, Gaussian-blurred."""
    rl, rt, rr, rb = [int(v) for v in rect]
    w, h = max(1, rr - rl), max(1, rb - rt)

    surf = _surface_for((rl, rt, rr, rb))
    ent = _blurred_screen(radius, surf)
    if ent is None:
        # no wallpaper file (solid colour / slideshow off / policy) — the
        # card still has to look like glass over *something*
        return Image.new("RGB", (w, h), _desktop_color())
    src, factor, ox, oy = ent

    iw, ih = src.size
    cl = int(round((rl - ox) / factor))
    ct = int(round((rt - oy) / factor))
    cw = max(1, int(round(w / factor)))
    ch = max(1, int(round(h / factor)))
    cl = max(0, min(cl, iw - 1))
    ct = max(0, min(ct, ih - 1))
    cr = max(cl + 1, min(cl + cw, iw))
    cb = max(ct + 1, min(ct + ch, ih))

    crop = src.crop((cl, ct, cr, cb))
    if crop.size != (w, h):
        crop = crop.resize((w, h), Image.Resampling.BILINEAR)
    return crop


def clear_cache() -> None:
    _scaled_cache.clear()
    _blur_screen.clear()
    _blur_order.clear()
    _img_cache["key"] = None
    _img_cache["img"] = None
