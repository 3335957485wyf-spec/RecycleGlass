"""Regenerate the app icon assets from a source PNG.

Trims the transparent margin (the source had ~11 % padding, which made the
glyph tiny at 50 px and illegible in the taskbar), then writes:
  assets/app-icon-1920.png  — master used by the widget card
  assets/app-icon.ico       — multi-size icon for the taskbar / tray / exe

The ICO is written by hand so each size can be sharpened individually;
Pillow's ICO writer would re-downsample every frame from the largest one.
"""
from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "recycle_glass" / "assets"
MASTER_SIDE = 1920
MARGIN = 0.05          # breathing room kept around the artwork
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def build_master(src: Path) -> Image.Image:
    im = Image.open(src).convert("RGBA")
    bbox = im.split()[3].getbbox()
    art = im.crop(bbox) if bbox else im
    side = max(art.size)
    pad = int(round(side * MARGIN))
    canvas = Image.new("RGBA", (side + pad * 2, side + pad * 2), (0, 0, 0, 0))
    canvas.alpha_composite(art, ((canvas.width - art.width) // 2,
                                 (canvas.height - art.height) // 2))
    return canvas.resize((MASTER_SIDE, MASTER_SIDE), Image.Resampling.LANCZOS)


# At 16-24 px the artwork's glyph is only ~37 % of the tile (≈9 px), which
# reads as a blue blob in the tray.  Small sizes get their own drawing: the
# tile fills the canvas and the glyph is scaled up to ~64 % with a bolder
# stroke, so it stays legible at 16 px.
TILE_TOP = (155, 191, 254)
TILE_BOTTOM = (180, 170, 251)
SMALL_MAX = 32
GLYPH_FRACTION = 0.66
GLYPH_STROKE = 2.35       # in 24-unit grid units (Lucide default is 2.0)


def small_frame(size: int) -> Image.Image:
    """Tray-sized icon: the real artwork, but with the glyph scaled up.

    The master draws the bin at ~42 % of the tile, which is 10 px at 24 px —
    unreadable in the tray.  Here the tile is redrawn at full bleed and the
    *actual* glyph (lifted from the source artwork) is pasted at 66 %.
    """
    ss = 8 if size <= 32 else 4
    n = size * ss
    tile = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    grad = Image.new("RGBA", (1, n))
    for y in range(n):
        u = y / max(1, n - 1)
        grad.putpixel((0, y), tuple(
            int(round(TILE_TOP[i] + (TILE_BOTTOM[i] - TILE_TOP[i]) * u))
            for i in range(3)) + (255,))
    grad = grad.resize((n, n), Image.Resampling.BILINEAR)

    mask = Image.new("L", (n * 2, n * 2), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, n * 2 - 1, n * 2 - 1), radius=int(n * 0.235), fill=255)
    mask = mask.resize((n, n), Image.Resampling.LANCZOS)
    tile.paste(grad, (0, 0), mask)

    glyph = Image.open(ASSETS / "glyph-trash.png").convert("RGBA")
    gw = max(1, int(round(n * GLYPH_FRACTION)))
    gh = max(1, int(round(gw * glyph.height / glyph.width)))
    g = glyph.resize((gw, gh), Image.Resampling.LANCZOS)

    # soft drop shadow, like the source artwork
    sh = Image.new("L", (gw, gh), 0)
    sh.paste(g.getchannel("A"), (0, 0))
    sh = sh.filter(ImageFilter.GaussianBlur(max(0.6, n * 0.012)))
    shadow = Image.new("RGBA", (gw, gh), (40, 60, 120, 0))
    shadow.putalpha(sh.point(lambda v: int(v * 0.35)))
    gx = (n - gw) // 2
    gy = (n - gh) // 2
    tile.alpha_composite(shadow, (gx, gy + max(1, int(n * 0.008))))
    tile.alpha_composite(g, (gx, gy))
    return tile.resize((size, size), Image.Resampling.LANCZOS)


def ico_frames(master: Image.Image):
    """Every size the shell may ask for, drawn for that size."""
    out = []
    for s in ICO_SIZES:
        if s <= SMALL_MAX:
            out.append((s, small_frame(s)))
            continue
        f = master.resize((s, s), Image.Resampling.LANCZOS)
        if s <= 48:
            # tiny sizes turn to mush without a little edge contrast
            f = f.filter(ImageFilter.UnsharpMask(radius=0.7, percent=70,
                                                 threshold=2))
        out.append((s, f))
    return out


def write_ico(frames, path: Path) -> None:
    blobs = []
    for size, im in frames:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append((size, buf.getvalue()))

    n = len(blobs)
    header = struct.pack("<HHH", 0, 1, n)
    offset = 6 + 16 * n
    entries = bytearray()
    data = bytearray()
    for size, blob in blobs:
        d = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", d, d, 0, 0, 1, 32,
                               len(blob), offset)
        offset += len(blob)
        data += blob
    path.write_bytes(bytes(header) + bytes(entries) + bytes(data))


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ASSETS / "app-icon-source.png"
    if not src.exists():
        print(f"source not found: {src}")
        return 1
    master = build_master(src)
    png = ASSETS / "app-icon-1920.png"
    master.save(png)
    ico = ASSETS / "app-icon.ico"
    write_ico(ico_frames(master), ico)
    print(f"master {master.size} -> {png.name} ({png.stat().st_size//1024} KB)")
    print(f"ico {ICO_SIZES} -> {ico.name} ({ico.stat().st_size//1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
