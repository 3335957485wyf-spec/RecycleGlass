"""Superellipse corners (squircles).

The app icon's frame is the Lamé curve ``|x|^n + |y|^n = 1`` with ``n = 3``.
Every UI surface uses the same corner curve: a box whose four corner arcs are
superellipse quarters instead of circular quarters.

Why it looks different: a circular arc meets the straight edge with a jump in
curvature (1/r -> 0), which the eye reads as a faint crease.  A superellipse
quarter leaves the edge with zero curvature, so the transition is continuous —
the corner looks "cast" rather than "folded".

The straight edges stay straight, so ``radius`` still says how big the corner
is; only the curve between edge and side changes.
"""

from __future__ import annotations

import math

#: Corner exponent.  2.0 = circular arc, 3.0 = the icon's curve, >4 = boxy.
N = 3.0

#: Points per corner quarter.  20 is plenty once the caller supersamples.
STEPS = 20


def _quarter(cx: float, cy: float, r: float, sx: int, sy: int,
             t0: float, t1: float, steps: int, e: float):
    pts = []
    for i in range(steps + 1):
        t = t0 + (t1 - t0) * i / steps
        u = abs(math.cos(t)) ** e
        v = abs(math.sin(t)) ** e
        pts.append((cx + sx * r * u, cy + sy * r * v))
    return pts


def polygon(w: float, h: float, radius: float, n: float | None = None,
            steps: int = STEPS) -> list[tuple[float, float]]:
    """Outline of a squircle-cornered box spanning ``[0,w] x [0,h]``.

    Coordinates are continuous (the shape's outer edge is exactly ``w``/``h``),
    so callers can pass already-supersampled sizes and rasterise directly.
    """
    n = N if n is None else n
    r = float(radius)
    r = min(max(r, 0.0), w / 2.0, h / 2.0)
    if r < 0.5:
        return [(0.0, 0.0), (w, 0.0), (w, h), (0.0, h)]

    e = 2.0 / n
    half_pi = math.pi / 2.0
    pts: list[tuple[float, float]] = []

    pts += _quarter(r, r, r, -1, -1, half_pi, 0.0, steps, e)      # top-left
    pts.append((0.0, h - r))
    pts += _quarter(r, h - r, r, -1, 1, 0.0, half_pi, steps, e)   # bottom-left
    pts.append((w - r, h))
    pts += _quarter(w - r, h - r, r, 1, 1, half_pi, 0.0, steps, e)  # bottom-right
    pts.append((w, r))
    pts += _quarter(w - r, r, r, 1, -1, 0.0, half_pi, steps, e)   # top-right
    pts.append((r, 0.0))
    return pts


def curve(w: float, h: float, radius: float, offset: tuple[float, float] = (0.0, 0.0),
          n: float | None = None, steps: int = STEPS):
    """``polygon`` translated by ``offset`` - handy for nested boxes."""
    ox, oy = offset
    return [(ox + x, oy + y) for x, y in polygon(w, h, radius, n=n, steps=steps)]
