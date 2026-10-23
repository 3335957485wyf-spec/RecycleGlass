"""Persistent settings for RecycleGlass.

Design rules learned the hard way:

* A half-written config must never brick the widget — writes go to a temp file
  and are swapped in with ``os.replace`` (atomic on NTFS).
* Every value is clamped on load.  A hand-edited ``"opacity": "abc"`` used to
  raise inside the render path, which killed the process with no log.
* Unknown keys are dropped instead of being copied onto the dataclass.
"""
from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass, fields
from pathlib import Path

from . import diag

APP_NAME = "RecycleGlass"
CONFIG_VERSION = 2


def config_dir() -> Path:
    base = Path(os.environ.get("APPDATA", str(Path.home())))
    d = base / APP_NAME
    try:
        d.mkdir(parents=True, exist_ok=True)
    except OSError:
        d = diag.data_dir()
    return d


def config_path() -> Path:
    return config_dir() / "config.json"


def _f(v, default: float, lo: float, hi: float) -> float:
    try:
        x = float(v)
    except (TypeError, ValueError):
        return default
    if x != x:  # NaN
        return default
    return max(lo, min(hi, x))


def _i(v, default: int, lo: int, hi: int) -> int:
    try:
        x = int(v)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, x))


def _b(v, default: bool) -> bool:
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return bool(v)
    if isinstance(v, str):
        return v.strip().lower() in ("1", "true", "yes", "on")
    return default


@dataclass
class Config:
    version: int = CONFIG_VERSION
    # geometry (virtual-screen coords)
    x: int = -1
    y: int = -1
    # appearance
    opacity: float = 0.78          # overall card opacity 0.20 .. 1.00
    material: float = 0.55         # tint strength over the blurred wallpaper
    size: str = "normal"           # small | normal | large
    blur_mode: str = "wallpaper"   # wallpaper | native
    blur_radius: float = 24.0      # gaussian radius in px
    theme: str = "system"          # "system" | "light" | "dark"
    auto_dpi: bool = False         # multiply the size preset by monitor DPI
    language: str = "auto"         # auto | zh | en
    # behaviour
    autostart: bool = False
    confirm_empty: bool = True
    # layout (derived at runtime, persisted only for diagnostics)
    width: int = 208
    height: int = 100
    # last known recycle-bin snapshot, so the card shows real numbers on the
    # first paint instead of waiting for the (slow) shell query
    last_count: int = 0
    last_size: int = 0

    # -- validation -----------------------------------------------------
    def _sanitise(self) -> None:
        if self.theme not in ("system", "light", "dark"):
            self.theme = "system"
        if self.blur_mode not in ("native", "wallpaper"):
            self.blur_mode = "wallpaper"
        # 2.1: the native (DWM) backdrop cannot coexist with the desktop layer,
        # it made the card disappear.  Migrate old configs silently.
        if self.blur_mode == "native":
            self.blur_mode = "wallpaper"
        if self.size not in ("small", "normal", "large"):
            self.size = "normal"
        if self.language not in ("auto", "zh", "en"):
            self.language = "auto"
        self.opacity = _f(self.opacity, 0.78, 0.20, 1.00)
        self.material = _f(self.material, 0.55, 0.0, 1.0)
        self.blur_radius = _f(self.blur_radius, 24.0, 0.0, 60.0)
        self.x = _i(self.x, -1, -32768, 32768)
        self.y = _i(self.y, -1, -32768, 32768)
        self.width = _i(self.width, 208, 64, 4096)
        self.height = _i(self.height, 100, 48, 4096)
        self.last_count = _i(self.last_count, 0, 0, 10_000_000)
        self.last_size = _i(self.last_size, 0, 0, 1 << 62)
        self.auto_dpi = _b(self.auto_dpi, False)
        self.autostart = _b(self.autostart, False)
        self.confirm_empty = _b(self.confirm_empty, True)
        self.version = CONFIG_VERSION

    # -- io -------------------------------------------------------------
    @classmethod
    def load(cls) -> "Config":
        cfg = cls()
        p = config_path()
        if not p.exists():
            return cfg
        try:
            raw = p.read_text(encoding="utf-8")
            data = json.loads(raw)
            if not isinstance(data, dict):
                raise ValueError("config root is not an object")
        except (OSError, ValueError, json.JSONDecodeError) as e:
            diag.warn("config unreadable (%s) — using defaults", e)
            try:
                p.replace(p.with_suffix(".broken.json"))
            except OSError:
                pass
            return cfg

        valid = {f.name for f in fields(cls)}
        for k, v in data.items():
            if k in valid:
                setattr(cfg, k, v)
        cfg._sanitise()
        return cfg

    def save(self) -> bool:
        """Atomic write: temp file in the same directory, then replace."""
        p = config_path()
        payload = json.dumps(asdict(self), indent=2, ensure_ascii=False)
        try:
            fd, tmp = tempfile.mkstemp(dir=str(p.parent), prefix=".cfg-",
                                       suffix=".tmp")
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    f.write(payload)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(tmp, p)
            finally:
                if os.path.exists(tmp):
                    try:
                        os.unlink(tmp)
                    except OSError:
                        pass
            return True
        except OSError as e:
            diag.warn("config save failed: %s", e)
            return False
