"""Timer-resolution helper for smooth animation.

Windows timers default to ~15.6 ms granularity, so a SetTimer(5) animation
actually steps at 64 Hz with uneven spacing — that uneven spacing is what
reads as judder.  Calling timeBeginPeriod(1) while an animation is running
brings the granularity down to ~1 ms; we release it as soon as everything
has settled so idle power use is unaffected.
"""
from __future__ import annotations

import ctypes

try:
    _winmm = ctypes.WinDLL("winmm")
except OSError:  # pragma: no cover
    _winmm = None

_held = 0


def begin() -> None:
    global _held
    _held += 1
    if _held == 1 and _winmm is not None:
        try:
            _winmm.timeBeginPeriod(1)
        except Exception:
            pass


def end() -> None:
    global _held
    if _held == 0:
        return
    _held -= 1
    if _held == 0 and _winmm is not None:
        try:
            _winmm.timeEndPeriod(1)
        except Exception:
            pass
