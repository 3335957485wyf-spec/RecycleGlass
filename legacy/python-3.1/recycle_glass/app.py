"""RecycleGlass — Fluent glass recycle-bin widget pinned to the Windows desktop.

Threading model: everything (window, timers, animation, menu, tray) runs on
the single UI thread inside the Win32 message loop.  Only the wallpaper
pre-blur and the menu warm-up ever leave it, and they never touch window
handles.
"""
from __future__ import annotations

import ctypes
import os
import queue
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image

from . import anim, bin as binapi, desktop, diag, droptarget, i18n, render
from . import screens, wallpaper
from . import menu as menu_mod
from . import w32
from .config import Config
from .w32 import (POINT, TRACKMOUSEEVENT, WNDCLASSEXW, WNDPROC, kernel32,
                  shell32, user32)

CLASS_NAME = "RecycleGlassWidget"
WM_APP_REFRESH = w32.WM_USER + 1
WM_APP_PULSE = w32.WM_USER + 2
WM_APP_BIN = w32.WM_USER + 3      # worker -> UI: (count, size)
WM_APP_OP = w32.WM_USER + 4       # worker -> UI: an operation finished

TIMER_POLL = 1
TIMER_FX = 2
TIMER_HOVER = 3
TIMER_TOAST = 4
TIMER_BUSY = 5

POLL_MS = 5000          # recycle-bin poll period (background thread)
QUERY_MIN_GAP = 2.0     # never run the shell query more often than this
DRAG_THRESHOLD = 4      # px before a press becomes a drag
FX_MS = 8               # ~125 Hz drop ripple
FX_DUR = 0.42
HOVER_MS = 10           # ~100 Hz hover cross-fade
HOVER_DUR = 0.16

# context-menu command ids
CMD_OPEN = 101
CMD_EMPTY = 102
CMD_SETTINGS = 103
CMD_AUTOSTART = 104
CMD_EXIT = 105
CMD_DPI = 106
CMD_CONFIRM_CANCEL = 201
CMD_CONFIRM_EMPTY = 202


def _ease_out(u: float) -> float:
    u = max(0.0, min(1.0, u))
    return 1.0 - (1.0 - u) ** 3


class QueryWorker(threading.Thread):
    """Polls the recycle bin off the UI thread.

    ``SHQueryRecycleBinW`` walks the whole bin: with ~19k items it takes ~15 s.
    Polling that every few seconds on the UI thread froze the card, and even on
    a worker it used to *block user actions* because everything shared one
    queue.  Now the poller is alone here, and the interval adapts to how long
    the query actually takes (15 s query → 60 s interval instead of 5 s).
    """

    MIN_GAP = 2.0
    MIN_INTERVAL = 4.0
    MAX_INTERVAL = 120.0

    def __init__(self, app: "App") -> None:
        super().__init__(daemon=True, name="RecycleGlassQuery")
        self.app = app
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._last: tuple[int, int] | None = None
        self.interval = 5.0

    def wake(self) -> None:
        self._wake.set()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()

    def run(self) -> None:
        try:
            ctypes.windll.ole32.CoInitializeEx(None, 0x2)
        except Exception:
            pass
        while not self._stop.is_set():
            self._wake.wait(self.interval)
            self._wake.clear()
            if self._stop.is_set():
                break
            t0 = time.monotonic()
            try:
                count, size = binapi.query()
            except Exception:
                diag.exception("recycle bin query failed")
                continue
            took = time.monotonic() - t0
            # a slow shell means a big bin: don't hammer it
            self.interval = max(self.MIN_INTERVAL,
                                min(self.MAX_INTERVAL, took * 4))
            if self._last == (count, size):
                continue
            self._last = (count, size)
            hwnd = self.app.hwnd
            if hwnd:
                user32.PostMessageW(hwnd, WM_APP_BIN, count, size)
        try:
            ctypes.windll.ole32.CoUninitialize()
        except Exception:
            pass


class ActionWorker(threading.Thread):
    """Runs user-initiated shell work (empty / recycle) with its own queue.

    Kept separate from the poller so a 15-second query can never delay a click.
    """

    def __init__(self, app: "App") -> None:
        super().__init__(daemon=True, name="RecycleGlassAction")
        self.app = app
        self.q: "queue.Queue[tuple]" = queue.Queue()
        self._stop = threading.Event()

    def post(self, *cmd) -> None:
        self.q.put(cmd)

    def stop(self) -> None:
        self._stop.set()
        self.q.put(("stop",))

    def run(self) -> None:
        try:
            ctypes.windll.ole32.CoInitializeEx(None, 0x2)
        except Exception:
            pass
        while not self._stop.is_set():
            cmd = self.q.get()
            if not cmd or cmd[0] == "stop":
                break
            try:
                if cmd[0] == "empty":
                    t0 = time.monotonic()
                    ok, remaining, size = binapi.empty(confirm=False)
                    diag.info("empty finished ok=%s remaining=%s in %.1fs",
                              ok, remaining, time.monotonic() - t0)
                    self.app.post_result(("empty", bool(ok), remaining, size))
                elif cmd[0] == "recycle":
                    paths = cmd[1]
                    ok, msg = binapi.recycle_paths(paths)
                    self.app.post_result(("recycle", ok, msg, len(paths)))
            except Exception:
                diag.exception("action worker failed")
        try:
            ctypes.windll.ole32.CoUninitialize()
        except Exception:
            pass


class App:
    def __init__(self) -> None:
        self.cfg = Config.load()
        i18n.set_language(self.cfg.language)
        self.hwnd = 0
        self.icon_cache: dict[bool, Image.Image | None] = {True: None, False: None}
        self.hover = False
        self.hover_btn = None
        self.dragging = False
        self._btn_down = False
        self.drag_origin = (0, 0)
        self.win_origin = (0, 0)
        self.mouse_down_pos = (0, 0)
        # shown immediately from the persisted snapshot, corrected by the
        # background query a moment later
        self.last_count = int(self.cfg.last_count)
        self.last_size = int(self.cfg.last_size)
        self.running = True
        self._tracking = False
        self._wproc = None  # keep the callback alive
        self._button_rects: list[tuple[str, int, int, int, int]] = []
        self._menu = None
        self._tray_id = 1
        self._tray_nid = None
        # bin-query cache: SHQueryRecycleBinW is a shell call and must not run
        # on every hover/repaint.
        self._last_sig = None
        self._cfg_dirty = False
        self._cfg_saved_at = 0.0
        self._drag_xy = None
        self._toast_text = ""
        self._toast_until = 0.0
        self._drag_over = False
        self._fx_t0 = 0.0
        self._drop_target = None
        self._ulw_fail = 0
        self.native = False
        self.strategy = "toplevel"
        # hover animation
        self.hover_t = 0.0
        self._h_from = 0.0
        self._h_to = 0.0
        self._h_t0 = 0.0
        self._h_dur = 0.0
        self._h_holding = False
        self.query_worker: QueryWorker | None = None
        self.action_worker: ActionWorker | None = None
        self._busy = ""
        self._busy_t0 = 0.0
        self._busy_total = 0
        self._busy_left = 0
        self._busy_tick = 0
        self._op_lock = threading.Lock()
        self._op_results: list[tuple] = []

    # ------------------------------------------------------------------
    # window plumbing
    # ------------------------------------------------------------------
    def create_window(self) -> None:
        hinst = kernel32.GetModuleHandleW(None)

        self._wproc = WNDPROC(self._wnd_proc)

        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.style = 0x0008  # CS_DBLCLKS
        wc.lpfnWndProc = ctypes.cast(self._wproc, ctypes.c_void_p).value
        wc.cbClsExtra = 0
        wc.cbWndExtra = 0
        wc.hInstance = hinst
        wc.hIcon = 0
        wc.hCursor = user32.LoadCursorW(None, ctypes.c_void_p(w32.IDC_ARROW))
        wc.hbrBackground = 0
        wc.lpszMenuName = None
        wc.lpszClassName = CLASS_NAME
        wc.hIconSm = 0

        atom = user32.RegisterClassExW(ctypes.byref(wc))
        err = ctypes.get_last_error()
        diag.info("RegisterClassExW atom=%s err=%s", atom, err)
        if not atom and err != 1410:  # ERROR_CLASS_ALREADY_EXISTS
            raise OSError(f"RegisterClassExW failed: {err}")

        self._apply_ui_scale(resize=False)
        w, h = render.WINDOW_W, render.WINDOW_H
        self.cfg.width = w
        self.cfg.height = h
        x, y = self._initial_pos(w, h)

        # Always a layered window.  A DWM backdrop needs a *non-layered*
        # window, and a non-layered top-level window parked behind Progman is
        # simply invisible — that is why switching to "native" made the card
        # vanish.  At the desktop layer only the wallpaper is behind us, so
        # the system backdrop had nothing to add anyway.
        self.native = False
        ex = w32.WS_EX_TOOLWINDOW | w32.WS_EX_NOACTIVATE | w32.WS_EX_LAYERED
        self.hwnd = user32.CreateWindowExW(
            ex, CLASS_NAME, "RecycleGlass", w32.WS_POPUP,
            x, y, w, h, 0, 0, hinst, None,
        )
        diag.info("CreateWindowExW hwnd=%s err=%s pos=(%s,%s) size=(%s,%s) "
                  "native=%s", self.hwnd, ctypes.get_last_error(),
                  x, y, w, h, self.native)
        if not self.hwnd:
            raise OSError(f"CreateWindowExW failed: {ctypes.get_last_error()}")

        self.strategy = desktop.attach_to_desktop(self.hwnd)
        # Now that we have a window, GetDpiForWindow is meaningful.  The scale
        # computed before creation used GetDpiForSystem, which reports 96 for a
        # per-monitor-v2 process — that is why "match display scaling" used to
        # do nothing until the window was dragged to another monitor.
        if self.cfg.auto_dpi:
            self._apply_ui_scale()
        desktop.reassert_zorder(self.hwnd, native=self.native)
        self._apply_window_icon()
        self._init_tray()
        shell32.DragAcceptFiles(self.hwnd, True)
        w32.allow_dropped_files(self.hwnd)
        # IDropTarget gives us DragEnter/DragLeave so the card can light up
        # while a file is still hovering over it.
        try:
            dt = droptarget.DropTarget(
                on_enter=self._on_drag_enter,
                on_leave=self._on_drag_leave,
                on_drop=self._on_ole_drop,
            )
            if dt.register(self.hwnd):
                self._drop_target = dt
                diag.info("OLE drop target registered")
            else:
                diag.info("OLE drop target unavailable; using WM_DROPFILES")
        except Exception:
            diag.exception("drop target setup failed")
        user32.SetTimer(self.hwnd, TIMER_POLL, POLL_MS, None)

        self._load_icons()
        self.query_worker = QueryWorker(self)
        self.query_worker.start()
        self.action_worker = ActionWorker(self)
        self.action_worker.start()
        try:
            self.refresh(redraw_backdrop=True, query=True)
        except Exception:
            diag.exception("first paint failed")
        diag.info("first paint done")

        # Warm the caches off-thread so the first right-click / hover is
        # instant (masks, icon sprites, fonts, shadows, AA patches).
        threading.Thread(target=_warmup, daemon=True).start()
        self._save_pos()

    def _initial_pos(self, w: int, h: int) -> tuple[int, int]:
        if self.cfg.x >= 0 and self.cfg.y >= 0:
            return screens.clamp_position(self.cfg.x, self.cfg.y, w, h)
        return screens.default_position(w, h)

    def _load_icons(self) -> None:
        """Load the custom app icon (replaces the system recycle-bin glyph)."""
        ico = render.load_app_icon()
        if ico:
            self.icon_cache[True] = ico
            self.icon_cache[False] = ico
        else:
            # fallback to the shell's stock icons if the asset is missing
            for full in (False, True):
                w, h, pixels = binapi.stock_icon_pixels(full)
                if w and h and pixels:
                    im = Image.new("RGBA", (w, h))
                    im.putdata(pixels)
                    self.icon_cache[full] = im

    def _icon_size(self, kind: str, fallback: int) -> int:
        """System icon metric so Windows gets the exact frame it will draw."""
        try:
            v = user32.GetSystemMetrics(
                w32.SM_CXICON if kind == "big" else w32.SM_CXSMICON)
            if 16 <= v <= 256:
                return int(v)
        except Exception:
            pass
        return fallback

    def _app_icon_path(self) -> str:
        return str(Path(__file__).resolve().parent / "assets" / "app-icon.ico")

    def _apply_window_icon(self) -> None:
        ico = Path(self._app_icon_path())
        if not ico.exists():
            return
        for kind, flag, fb in (("big", w32.ICON_BIG, 32),
                               ("small", w32.ICON_SMALL, 16)):
            sz = self._icon_size(kind, fb)
            h = user32.LoadImageW(None, str(ico), 1, sz, sz, 0x0010)
            if h:
                user32.SendMessageW(self.hwnd, w32.WM_SETICON, flag, h)

    # ------------------------------------------------------------------
    # system tray
    # ------------------------------------------------------------------
    def _init_tray(self) -> None:
        ico_path = self._app_icon_path()
        hicon = 0
        if Path(ico_path).exists():
            tray_sz = self._icon_size("small", 16)
            hicon = user32.LoadImageW(None, ico_path, 1, tray_sz, tray_sz, 0x0010)
        if not hicon:
            hicon = user32.LoadIconW(None, ctypes.c_void_p(32512))

        nid = w32.NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(w32.NOTIFYICONDATAW)
        nid.hWnd = self.hwnd
        nid.uID = self._tray_id
        nid.uFlags = w32.NIF_MESSAGE | w32.NIF_ICON | w32.NIF_TIP
        nid.uCallbackMessage = w32.WM_APP_TRAY
        nid.hIcon = hicon
        nid.szTip = "RecycleGlass"
        ok = shell32.Shell_NotifyIconW(w32.NIM_ADD, ctypes.byref(nid))
        self._tray_nid = nid
        diag.info("tray added ok=%s", ok)
        if self.last_count >= 0:
            self._update_tray_tip(max(0, self.last_count), max(0, self.last_size))

    def _update_tray_tip(self, count: int, size: int) -> None:
        if not self._tray_nid:
            return
        nid = self._tray_nid
        nid.uFlags = w32.NIF_TIP
        if count:
            nid.szTip = i18n.t("tray_count", n=count,
                               size=binapi.format_size(size))[:127]
        else:
            nid.szTip = i18n.t("tray_empty")
        shell32.Shell_NotifyIconW(w32.NIM_MODIFY, ctypes.byref(nid))

    def _remove_tray(self) -> None:
        if self._tray_nid:
            self._tray_nid.uFlags = 0
            shell32.Shell_NotifyIconW(w32.NIM_DELETE, ctypes.byref(self._tray_nid))
            self._tray_nid = None

    def _on_tray_msg(self, lparam: int) -> None:
        msg = lparam & 0xFFFF
        if msg in (w32.WM_LBUTTONUP_TRAY, w32.WM_LBUTTONDBLCLK_TRAY):
            binapi.open_bin()
        elif msg == w32.WM_RBUTTONUP_TRAY:
            self._show_menu()

    # ------------------------------------------------------------------
    # painting
    # ------------------------------------------------------------------
    def _request_query(self) -> None:
        """Ask the poller for a fresh count (never blocks the UI)."""
        if self.query_worker is not None:
            self.query_worker.wake()

    def post_result(self, item: tuple) -> None:
        """Called from a worker thread: queue a result and poke the UI."""
        with self._op_lock:
            self._op_results.append(item)
        hwnd = self.hwnd
        if hwnd:
            user32.PostMessageW(hwnd, WM_APP_OP, 0, 0)

    def _subtitle(self) -> str:
        now_t = time.monotonic()
        if self._busy:
            secs = max(0, int(now_t - self._busy_t0))
            if self._busy == "empty" and 0 < self._busy_left < self._busy_total:
                return i18n.t("busy_empty_progress", n=self._busy_left)
            key = "busy_empty" if self._busy == "empty" else "busy_recycle"
            return i18n.t(key, s=secs)
        if self._drag_over:
            return i18n.t("drop_hint")
        if self._toast_text and now_t < self._toast_until:
            return self._toast_text
        count, size = self.last_count, self.last_size
        if count > 0:
            return i18n.t("sub_count", n=count, size=binapi.format_size(size))
        return i18n.t("sub_empty")

    def _toast(self, text: str, seconds: float = 2.5) -> None:
        self._toast_text = text
        self._toast_until = time.monotonic() + seconds
        if self.hwnd:
            # repaint exactly when the message expires, instead of waiting for
            # the 5 s poll tick
            user32.SetTimer(self.hwnd, TIMER_TOAST, max(120, int(seconds * 1000)),
                            None)
        self.refresh()

    def refresh(self, redraw_backdrop: bool = False, query: bool = False) -> None:
        if not self.hwnd:
            return
        if self.dragging and not redraw_backdrop:
            return
        if query:
            self._request_query()
        count, size = self.last_count, self.last_size

        rect = w32.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(rect))
        # while dragging we render for the *target* position and let
        # UpdateLayeredWindow move + repaint in one go — moving the window
        # first and repainting a frame later caused a jelly effect
        left = self._drag_xy[0] if self._drag_xy else rect.left
        top = self._drag_xy[1] if self._drag_xy else rect.top
        win_w = rect.right - rect.left
        win_h = rect.bottom - rect.top
        if win_w <= 0 or win_h <= 0:
            win_w, win_h = render.WINDOW_W, render.WINDOW_H

        cache_key = (left, top, win_w, win_h, self.cfg.blur_radius,
                     self.cfg.theme, self.native)
        need_bd = (redraw_backdrop
                   or cache_key != getattr(self, "_bd_key", None)
                   or (not self.native
                       and getattr(self, "_bd_cache", None) is None))
        if need_bd:
            card = (
                left + render.PAD,
                top + render.PAD,
                left + render.PAD + render.CARD_W,
                top + render.PAD + render.CARD_H,
            )
            self._bd_cache = self._live_backdrop(card)
            self._bd_key = cache_key

        pal = render.palette_for(self.cfg.theme)
        full = count > 0
        now_t = time.monotonic()
        fx_t = None
        if self._fx_t0:
            ft = (now_t - self._fx_t0) / FX_DUR
            if 0.0 <= ft < 1.0:
                fx_t = ft
        frame_key = int(fx_t * 120) if fx_t is not None else -1
        hover_q = int(round(self.hover_t * 24))

        subtitle = self._subtitle()
        # Two different questions:
        #  * hit testing keys on the logical hover flag, so a quick click lands
        #    on a pill that is still fading in;
        #  * drawing keeps the pills alive while hover_t is still draining,
        #    otherwise releasing the mouse snapped them (and the title) back
        #    instantly instead of animating out.
        can_hover = (not self._drag_over) and not self._busy
        hit_hover = can_hover and self.hover
        draw_hover = can_hover and (self.hover or self.hover_t > 0.01)

        # --- dirty check -------------------------------------------------
        # Repainting identical pixels is the main idle CPU cost.
        sig = (
            count, size, draw_hover, self.hover_btn, full, subtitle,
            self._drag_over, frame_key, hover_q,
            self.cfg.opacity, self.cfg.material, self.cfg.theme,
            self.cfg.blur_radius, i18n.language(),
            left, top, win_w, win_h,
            id(getattr(self, "_bd_cache", None)),
        )
        if sig == self._last_sig and not redraw_backdrop:
            return
        self._last_sig = sig

        img, hits = render.render(
            (win_w, win_h),
            getattr(self, "_bd_cache", None),
            self.icon_cache.get(full),
            i18n.t("app_name"),
            subtitle,
            draw_hover,
            pal,
            self.cfg.opacity,
            self.cfg.material,
            hover_buttons=(i18n.t("btn_open"), i18n.t("btn_empty")),
            hover_btn=self.hover_btn if hit_hover else None,
            drag_hover=self._drag_over,
            fx_t=fx_t,
            native=self.native,
            hover_t=self.hover_t,
        )
        self._button_rects = hits if hit_hover else []
        bgra = render.to_bgra(img)
        self._last_bgra = bgra
        if self.native:
            if self._drag_xy is not None:
                user32.SetWindowPos(self.hwnd, 0, left, top, 0, 0,
                                    w32.SWP_NOSIZE | w32.SWP_NOZORDER
                                    | w32.SWP_NOACTIVATE)
            desktop.present(self.hwnd, win_w, win_h, left, top, bgra,
                            native=True)
        else:
            ok = desktop.update_layered(self.hwnd, win_w, win_h, left, top, bgra)
            if ok:
                self._ulw_fail = 0
            else:
                self._ulw_fail += 1
                if self._ulw_fail in (5, 30, 120):
                    diag.warn("UpdateLayeredWindow failed %s times in a row",
                              self._ulw_fail)

    def _on_bin_result(self, count: int, size: int) -> None:
        """Worker finished a query (may arrive long after the request)."""
        if (count, size) == (self.last_count, self.last_size):
            return
        if self._busy == "empty" and count == 0:
            # a zero here can also mean "the query failed while the shell was
            # busy deleting" — let the verified result decide instead
            return
        was_empty = self.last_count == 0
        if self._busy == "empty":
            self._busy_left = count
        self.last_count, self.last_size = count, size
        if count == 0 and not was_empty:
            self._load_icons()
        self._update_tray_tip(count, size)
        self.cfg.last_count, self.cfg.last_size = count, size
        self._cfg_dirty = True
        self.refresh()

    def _on_op_done(self) -> None:
        """A recycle / empty operation finished on a worker thread."""
        with self._op_lock:
            results, self._op_results = self._op_results, []
        for item in results:
            kind = item[0]
            if kind == "recycle":
                _k, ok, msg, n = item
                self._busy = ""
                self._busy_t0 = 0.0
                self._toast(i18n.t("toast_recycled", n=n) if ok else msg)
                self._load_icons()
                self._start_fx()
            elif kind == "empty":
                self._busy = ""
                self._busy_t0 = 0.0
                ok = bool(item[1])
                remaining = int(item[2]) if len(item) > 2 else 0
                size = int(item[3]) if len(item) > 3 else 0
                self.last_count, self.last_size = remaining, size
                self.cfg.last_count, self.cfg.last_size = remaining, size
                self._busy_left = remaining
                if ok:
                    self._toast(i18n.t("toast_empty_done"))
                elif remaining < self._busy_total:
                    self._toast(i18n.t("toast_partial", n=remaining), 3.5)
                else:
                    self._toast(i18n.t("toast_failed"), 3.0)
                self._load_icons()
                self._start_fx()
        if self.hwnd:
            user32.KillTimer(self.hwnd, TIMER_BUSY)
        self._last_sig = None
        self._request_query()
        self.refresh()

    # ------------------------------------------------------------------
    # hover animation
    # ------------------------------------------------------------------
    def _set_hover(self, value: bool) -> None:
        if value == self.hover:
            return
        self.hover = value
        if not value:
            self.hover_btn = None
        self._animate_hover(1.0 if value else 0.0)
        self.refresh()          # publish the pill hit rects right away

    def _animate_hover(self, target: float) -> None:
        if not self.hwnd:
            self.hover_t = target
            return
        if abs(self.hover_t - target) < 0.004:
            self.hover_t = target
            self.refresh()
            return
        self._h_from = self.hover_t
        self._h_to = target
        self._h_t0 = time.monotonic()
        self._h_dur = HOVER_DUR
        if not self._h_holding:
            self._h_holding = True
            anim.begin()
        user32.SetTimer(self.hwnd, TIMER_HOVER, HOVER_MS, None)

    def _tick_hover(self) -> None:
        if self._h_dur <= 0:
            self._stop_hover()
            return
        u = (time.monotonic() - self._h_t0) / self._h_dur
        if u >= 1.0:
            self.hover_t = self._h_to
            self._stop_hover()
            self.refresh()
            return
        self.hover_t = self._h_from + (self._h_to - self._h_from) * _ease_out(u)
        self.refresh()

    def _stop_hover(self) -> None:
        if self.hwnd:
            user32.KillTimer(self.hwnd, TIMER_HOVER)
        self._h_dur = 0.0
        if self._h_holding:
            self._h_holding = False
            anim.end()

    # ------------------------------------------------------------------
    # interaction
    # ------------------------------------------------------------------
    def _hit_buttons(self, x: int, y: int) -> str | None:
        for label, x0, y0, x1, y1 in self._button_rects:
            if x0 <= x <= x1 and y0 <= y <= y1:
                return "open" if label == i18n.t("btn_open") else "empty"
        return None

    def _hit_button_label(self, x: int, y: int) -> str | None:
        for label, x0, y0, x1, y1 in self._button_rects:
            if x0 <= x <= x1 and y0 <= y <= y1:
                return label
        return None

    def _on_lbutton_down(self, x: int, y: int) -> None:
        self.mouse_down_pos = (x, y)
        pt = POINT(0, 0)
        user32.GetCursorPos(ctypes.byref(pt))
        self.drag_origin = (pt.x, pt.y)          # SCREEN coords
        rect = w32.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(rect))
        self.win_origin = (rect.left, rect.top)   # SCREEN coords
        self.dragging = False
        self._btn_down = True
        user32.SetCapture(self.hwnd)

    def _on_lbutton_up(self, x: int, y: int) -> None:
        user32.ReleaseCapture()
        self._btn_down = False
        was_drag = self.dragging
        self.dragging = False
        if was_drag:
            self._drag_xy = None
            self._snap_into_screen()
            self._save_pos()
            self.refresh(redraw_backdrop=True, query=True)
            return
        hit = self._hit_buttons(x, y)
        if hit == "open":
            binapi.open_bin()
        elif hit == "empty":
            self._do_empty()

    def _on_lbutton_dblclk(self, x: int, y: int) -> None:
        """Double-click opens the bin, like the desktop icon."""
        if self._hit_buttons(x, y):
            return          # the pill handles its own action
        binapi.open_bin()

    def _on_mouse_move(self, x: int, y: int) -> None:
        if not self._tracking:
            tme = TRACKMOUSEEVENT()
            tme.cbSize = ctypes.sizeof(TRACKMOUSEEVENT)
            tme.dwFlags = w32.TME_LEAVE
            tme.hwndTrack = self.hwnd
            tme.dwHoverTime = 0
            user32.TrackMouseEvent(ctypes.byref(tme))
            self._tracking = True

        if self._btn_down and (user32.GetAsyncKeyState(w32.VK_LBUTTON) & 0x8000):
            pt = POINT(0, 0)
            user32.GetCursorPos(ctypes.byref(pt))
            dx = abs(pt.x - self.drag_origin[0])
            dy = abs(pt.y - self.drag_origin[1])
            if not self.dragging and (dx > DRAG_THRESHOLD or dy > DRAG_THRESHOLD):
                self.dragging = True
            if self.dragging:
                nx = self.win_origin[0] + (pt.x - self.drag_origin[0])
                ny = self.win_origin[1] + (pt.y - self.drag_origin[1])
                self._drag_xy = (nx, ny)
                self.refresh(redraw_backdrop=True)
                return
        elif self._btn_down:
            self._btn_down = False
            self.dragging = False

        new_hover = (render.PAD <= x <= render.PAD + render.CARD_W
                     and render.PAD <= y <= render.PAD + render.CARD_H)
        if new_hover != self.hover:
            self._set_hover(new_hover)
        if self.hover:
            btn = self._hit_button_label(x, y)
            if btn != self.hover_btn:
                self.hover_btn = btn
                self.refresh()

    def _on_mouse_leave(self) -> None:
        self._tracking = False
        self.hover_btn = None
        if self.hover:
            self._set_hover(False)

    def _do_empty(self) -> None:
        diag.info("empty requested busy=%r count=%s confirm=%s", self._busy,
                  self.last_count, self.cfg.confirm_empty)
        if self.last_count == 0:
            self._toast(i18n.t("toast_already_empty"), 1.8)
            return
        if self._busy:
            return
        if self.cfg.confirm_empty:
            # our own flyout instead of SHEmptyRecycleBin's prompt: that dialog
            # is created on a worker thread with no owner, so it could open
            # behind other windows (or not visibly at all) — which is what made
            # the button feel dead.
            self._show_confirm()
            return
        self._start_empty()

    def _start_empty(self) -> None:
        """Kick off the deletion on the action thread (may take minutes)."""
        if self._busy:
            return
        self._set_busy("empty")
        if self.action_worker is not None:
            self.action_worker.post("empty")
        else:
            self._busy = ""
            self._toast(i18n.t("toast_failed"), 2.0)

    def _show_confirm(self) -> None:
        items = menu_mod.confirm_items()
        if self.cfg.theme == "light":
            dark = False
        elif self.cfg.theme == "dark":
            dark = True
        else:
            dark = not render.system_is_light()
        hint = i18n.t("confirm_hint",
                      n=self.last_count,
                      size=binapi.format_size(self.last_size))
        pt = POINT(0, 0)
        user32.GetCursorPos(ctypes.byref(pt))
        self._menu = menu_mod.MenuWindow(
            items, self._on_command, dark,
            status_text=hint, app_icon=render.load_app_icon(),
            settings={
                "opacity": int(round(self.cfg.opacity * 100)),
                "material": int(round(self.cfg.material * 100)),
                "blur": int(round(self.cfg.blur_radius)),
                "theme": self.cfg.theme,
                "size": self.cfg.size,
            },
            on_setting=self._on_menu_setting,
            blur_radius=self.cfg.blur_radius,
            header_title=i18n.t("confirm_title"),
        )
        self._menu.popup(pt.x, pt.y)

    def _set_busy(self, kind: str) -> None:
        """Long shell work: show a live counter so the card never looks dead."""
        self._busy = kind
        self._busy_t0 = time.monotonic()
        self._busy_total = max(0, self.last_count)
        self._busy_left = self._busy_total
        self._busy_tick = 0
        if self.hwnd:
            user32.SetTimer(self.hwnd, TIMER_BUSY, 1000, None)
        self._last_sig = None
        self.refresh()

    def _snap_into_screen(self) -> None:
        """Keep the card fully visible after a drag / monitor change."""
        rect = w32.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        x, y = screens.clamp_position(rect.left, rect.top, w, h)
        if (x, y) != (rect.left, rect.top):
            self._drag_xy = None
            user32.SetWindowPos(self.hwnd, 0, x, y, 0, 0,
                                w32.SWP_NOSIZE | w32.SWP_NOZORDER
                                | w32.SWP_NOACTIVATE)

    def _save_pos(self) -> None:
        rect = w32.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(rect))
        self.cfg.x = rect.left
        self.cfg.y = rect.top
        self.cfg.width = rect.right - rect.left
        self.cfg.height = rect.bottom - rect.top
        self.cfg.last_count = max(0, self.last_count)
        self.cfg.last_size = max(0, self.last_size)
        self.cfg.save()
        self._cfg_dirty = False
        self._cfg_saved_at = time.monotonic()

    # ------------------------------------------------------------------
    # menu
    # ------------------------------------------------------------------
    def _show_menu(self) -> None:
        self._do_show_menu()

    def _do_show_menu(self, expanded: bool = False) -> None:
        empty = self.last_count == 0
        items = menu_mod.default_items(self.cfg.autostart, empty=empty,
                                       auto_dpi=self.cfg.auto_dpi)
        if self.cfg.theme == "light":
            dark = False
        elif self.cfg.theme == "dark":
            dark = True
        else:
            dark = not render.system_is_light()
        if empty:
            status = i18n.t("status_empty")
        else:
            status = f"{binapi.format_count(self.last_count)} · " \
                     f"{binapi.format_size(self.last_size)}"
        app_icon = render.load_app_icon()
        pt = POINT(0, 0)
        user32.GetCursorPos(ctypes.byref(pt))
        settings = {
            "opacity": int(round(self.cfg.opacity * 100)),
            "material": int(round(self.cfg.material * 100)),
            "blur": int(round(self.cfg.blur_radius)),
            "theme": self.cfg.theme,
            "size": self.cfg.size,
            "blur_mode": self.cfg.blur_mode,
        }
        self._menu = menu_mod.MenuWindow(
            items, self._on_command, dark,
            status_text=status, app_icon=app_icon,
            settings=settings, on_setting=self._on_menu_setting,
            blur_radius=self.cfg.blur_radius,
        )
        self._menu.popup(pt.x, pt.y, expanded=expanded)

    def _ui_scale(self) -> float:
        """Size preset, optionally multiplied by the monitor's DPI factor.

        Matching the display scaling is what makes the widget the same
        *apparent* size on a 150 % laptop and a 100 % desktop; it is opt-in
        because some users prefer the compact fixed-pixel look.
        """
        base = menu_mod.SIZE_SCALES.get(self.cfg.size, 1.0)
        if not self.cfg.auto_dpi:
            return base
        dpi = screens.dpi_for_window(self.hwnd) if self.hwnd else 96
        # Sub-linear on purpose: a proportional dpi/96 factor made the card
        # 1.5x on a 150 % screen, which read as "way too big".  The exponent is
        # calibrated so 150 % lands on 1.18 — the size people were already
        # happy with — while 100 % stays exactly the base size and 200 % grows
        # to ~1.32 instead of 2.0.
        f = max(0.85, min(1.6, (dpi / 96.0) ** 0.4))
        return max(0.6, min(2.2, base * f))

    def _live_backdrop(self, rect):
        """Backdrop for the card.

        Screen capture was tried here, but BitBlt only returns the desktop
        surface: desktop icons are included while the taskbar, other windows
        and our own layered window are not.  Blurring the wallpaper is both
        correct and cheap; true live blur needs a DWM backdrop, which is
        incompatible with the per-pixel alpha this widget relies on.
        """
        if self.native:
            return None
        return wallpaper.blurred_backdrop(rect, radius=self.cfg.blur_radius)

    def _apply_ui_scale(self, resize: bool = True) -> None:
        scale = self._ui_scale()
        render.set_scale(scale)
        menu_mod.set_scale(scale)
        if not resize or not self.hwnd:
            return
        w, h = render.WINDOW_W, render.WINDOW_H
        rect = w32.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(rect))
        x, y = screens.clamp_position(rect.left, rect.top, w, h)
        user32.SetWindowPos(self.hwnd, 0, x, y, w, h,
                            w32.SWP_NOZORDER | w32.SWP_NOACTIVATE)
        self._bd_cache = None
        self._bd_key = None
        self._last_sig = None
        self.refresh(redraw_backdrop=True, query=True)

    def _on_menu_setting(self, key: str, value) -> None:
        if key == "opacity":
            self.cfg.opacity = max(0.2, min(1.0, value / 100))
            self.refresh()
        elif key == "material":
            self.cfg.material = max(0.0, min(1.0, value / 100))
            self.refresh()
        elif key == "blur":
            self.cfg.blur_radius = max(0.0, min(60.0, float(value)))
            wallpaper.clear_cache()
            self.refresh(redraw_backdrop=True)
        elif key == "theme":
            self.cfg.theme = value
            self._last_sig = None
            self.refresh(redraw_backdrop=False)
        elif key == "size":
            self.cfg.size = value
            if self._menu and self._menu.is_open:
                self._menu.close(animate=False)
            self._apply_ui_scale()
        elif key == "dpi":
            self.cfg.auto_dpi = bool(value)
            if self._menu and self._menu.is_open:
                self._menu.close(animate=False)
            self._apply_ui_scale()
        self._cfg_dirty = True

    def _on_command(self, cmd: int) -> None:
        diag.info("command %s", cmd)
        if cmd == CMD_OPEN:
            binapi.open_bin()
        elif cmd == CMD_EMPTY:
            self._do_empty()
        elif cmd == CMD_SETTINGS:
            self._do_show_menu(expanded=True)
        elif cmd == CMD_AUTOSTART:
            self.cfg.autostart = not self.cfg.autostart
            apply_autostart(self.cfg.autostart)
            self.cfg.save()
        elif cmd == CMD_DPI:
            self.cfg.auto_dpi = not self.cfg.auto_dpi
            if self._menu and self._menu.is_open:
                self._menu.close(animate=False)
            self._apply_ui_scale()
            self.cfg.save()
        elif cmd == CMD_CONFIRM_CANCEL:
            pass
        elif cmd == CMD_CONFIRM_EMPTY:
            self._start_empty()
        elif cmd == CMD_EXIT:
            self.running = False
            user32.PostMessageW(self.hwnd, w32.WM_CLOSE, 0, 0)

    # ------------------------------------------------------------------
    # window procedure
    # ------------------------------------------------------------------
    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        try:
            return self._handle(hwnd, msg, wparam, lparam)
        except Exception:
            diag.exception(f"WNDPROC error msg=0x{msg:04X}")
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _handle(self, hwnd, msg, wparam, lparam):
        if msg == w32.WM_MOUSEACTIVATE:
            return w32.MA_NOACTIVATE

        if msg == w32.WM_MOUSEMOVE:
            self._on_mouse_move(ctypes.c_short(lparam & 0xFFFF).value,
                                ctypes.c_short((lparam >> 16) & 0xFFFF).value)
            return 0

        if msg == w32.WM_MOUSELEAVE:
            self._on_mouse_leave()
            return 0

        if msg == 0x0215:  # WM_CAPTURECHANGED
            self._btn_down = False
            if self.dragging:
                self.dragging = False
                self._drag_xy = None
                self.refresh(redraw_backdrop=True)
            return 0

        if msg == w32.WM_LBUTTONDOWN:
            self._on_lbutton_down(ctypes.c_short(lparam & 0xFFFF).value,
                                  ctypes.c_short((lparam >> 16) & 0xFFFF).value)
            return 0

        if msg == w32.WM_LBUTTONUP:
            self._on_lbutton_up(ctypes.c_short(lparam & 0xFFFF).value,
                                ctypes.c_short((lparam >> 16) & 0xFFFF).value)
            return 0

        if msg == w32.WM_LBUTTONDBLCLK:
            self._on_lbutton_dblclk(ctypes.c_short(lparam & 0xFFFF).value,
                                    ctypes.c_short((lparam >> 16) & 0xFFFF).value)
            return 0

        if msg == w32.WM_RBUTTONUP:
            self._show_menu()
            return 0

        if msg == w32.WM_COMMAND:
            self._on_command(wparam & 0xFFFF)
            return 0

        if msg == w32.WM_DROPFILES:
            self._on_drop(wparam)
            return 0

        if msg == w32.WM_APP_TRAY:
            self._on_tray_msg(lparam)
            return 0

        if msg == WM_APP_PULSE:
            self._start_fx()
            return 0

        if msg == WM_APP_BIN:
            self._on_bin_result(int(wparam), int(lparam))
            return 0

        if msg == WM_APP_OP:
            self._on_op_done()
            return 0

        if msg == _TASKBAR_CREATED and _TASKBAR_CREATED:
            # Explorer restarted: the tray icon is gone, re-add it and
            # re-assert the desktop z-order (the shell rebuilt its windows).
            diag.info("TaskbarCreated — re-adding tray icon")
            self._remove_tray()
            self._init_tray()
            desktop.reassert_zorder(self.hwnd, native=self.native)
            return 0

        if msg == 0x000F:  # WM_PAINT
            ps = w32.PAINTSTRUCT()
            hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
            if self.native and getattr(self, "_last_bgra", None):
                desktop.blit_bgra(hdc, render.WINDOW_W, render.WINDOW_H,
                                  self._last_bgra, 255)
            user32.EndPaint(hwnd, ctypes.byref(ps))
            return 0

        if msg == 0x02E0:  # WM_DPICHANGED — moved to another monitor
            if self.cfg.auto_dpi:
                self._apply_ui_scale()
            else:
                self._snap_into_screen()
                wallpaper.clear_cache()
                self.refresh(redraw_backdrop=True, query=True)
            return 0

        if msg == w32.WM_TIMER:
            if wparam == TIMER_FX:
                self._tick_fx()
                return 0
            if wparam == TIMER_HOVER:
                self._tick_hover()
                return 0
            if wparam == TIMER_BUSY:
                self._busy_tick += 1
                if self._busy == "empty" and self._busy_tick % 4 == 0:
                    self._request_query()
                self.refresh()
                return 0
            if wparam == TIMER_TOAST:
                user32.KillTimer(self.hwnd, TIMER_TOAST)
                self.refresh()
                return 0
            if wparam == TIMER_POLL:
                self.refresh()
                desktop.reassert_zorder(self.hwnd, native=self.native)
                if self._cfg_dirty and time.monotonic() - self._cfg_saved_at > 1.0:
                    self.cfg.save()
                    self._cfg_dirty = False
                    self._cfg_saved_at = time.monotonic()
            return 0

        if msg == w32.WM_SETTINGCHANGE:
            wallpaper.clear_cache()
            self._last_sig = None
            self.refresh(redraw_backdrop=True, query=True)
            return 0

        if msg == w32.WM_DISPLAYCHANGE:
            wallpaper.clear_cache()
            self._snap_into_screen()
            self._last_sig = None
            self.refresh(redraw_backdrop=True, query=True)
            return 0

        if msg == w32.WM_MOVE:
            if not self.dragging:
                self.refresh()
            return 0

        if msg == w32.WM_CLOSE:
            self.running = False
            self._save_pos()
            user32.DestroyWindow(hwnd)
            return 0

        if msg == w32.WM_DESTROY:
            for w in (self.query_worker, self.action_worker):
                if w is not None:
                    try:
                        w.stop()
                    except Exception:
                        pass
            self.query_worker = None
            self.action_worker = None
            if self._drop_target is not None:
                try:
                    self._drop_target.revoke()
                except Exception:
                    pass
                self._drop_target = None
            user32.KillTimer(hwnd, TIMER_FX)
            user32.KillTimer(hwnd, TIMER_HOVER)
            if self._fx_t0:
                self._fx_t0 = 0.0
            self._stop_hover()
            anim.end()
            user32.KillTimer(hwnd, TIMER_POLL)
            user32.KillTimer(hwnd, TIMER_TOAST)
            self._remove_tray()
            # recreating the window (blur-mode switch) must NOT end the loop
            if not self.running:
                user32.PostQuitMessage(0)
            return 0

        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    # ------------------------------------------------------------------
    # drag & drop
    # ------------------------------------------------------------------
    def _on_drag_enter(self) -> None:
        if not self._drag_over:
            self._drag_over = True
            self._set_hover(True)
            self.refresh()

    def _on_drag_leave(self) -> None:
        if self._drag_over:
            self._drag_over = False
            self.refresh()

    def _on_ole_drop(self, paths) -> None:
        self._drag_over = False
        self._handle_dropped(paths)

    def _handle_dropped(self, paths) -> None:
        diag.info("drop: %s path(s)", len(paths))
        if not paths:
            self._toast(i18n.t("toast_nothing"), 2.0)
            return
        if self.action_worker is not None:
            self._set_busy("recycle")
            self.action_worker.post("recycle", list(paths))
        else:
            ok, msg = binapi.recycle_paths(paths)
            self._toast(i18n.t("toast_recycled", n=len(paths)) if ok else msg)
            self._load_icons()
            self._start_fx()
        self.refresh()

    def _start_fx(self) -> None:
        if not self.hwnd:
            return
        self._fx_t0 = time.monotonic()
        anim.begin()
        user32.SetTimer(self.hwnd, TIMER_FX, FX_MS, None)
        self.refresh()

    def _tick_fx(self) -> None:
        if not self._fx_t0:
            return
        if time.monotonic() - self._fx_t0 >= FX_DUR:
            user32.KillTimer(self.hwnd, TIMER_FX)
            self._fx_t0 = 0.0
            anim.end()
        self.refresh()

    def _on_drop(self, hdrop) -> None:
        count = shell32.DragQueryFileW(hdrop, 0xFFFFFFFF, None, 0)
        paths: list[str] = []
        for i in range(count):
            buf = ctypes.create_unicode_buffer(1024)
            shell32.DragQueryFileW(hdrop, i, buf, 1024)
            if buf.value:
                paths.append(buf.value)
        shell32.DragFinish(hdrop)
        self._handle_dropped(paths)

    # ------------------------------------------------------------------
    # run loop
    # ------------------------------------------------------------------
    def run(self) -> None:
        msg = wintypes.MSG()
        # Blocking GetMessage instead of PeekMessage + sleep: the widget is
        # timer driven, so an idle card now costs literally zero CPU.
        while self.running:
            r = user32.GetMessageW(ctypes.byref(msg), 0, 0, 0)
            if r in (0, -1):        # WM_QUIT or error
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def apply_autostart(enabled: bool) -> bool:
    """Create/remove the HKCU Run entry. Returns True on success."""
    import winreg

    key = r"Software\Microsoft\Windows\CurrentVersion\Run"
    name = "RecycleGlass"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0,
                            winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE) as k:
            if enabled:
                if getattr(sys, "frozen", False):
                    cmd = f'"{sys.executable}"'
                else:
                    py = sys.executable
                    main = str(Path(__file__).resolve().parent.parent / "run.py")
                    cmd = f'"{py}" "{main}"'
                winreg.SetValueEx(k, name, 0, winreg.REG_SZ, cmd)
            else:
                try:
                    winreg.DeleteValue(k, name)
                except OSError:
                    pass
        return True
    except OSError as e:
        diag.warn("autostart update failed: %s", e)
        return False


def autostart_matches(enabled: bool) -> bool:
    """True when the registry already reflects the requested state."""
    import winreg

    key = r"Software\Microsoft\Windows\CurrentVersion\Run"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
            val, _ = winreg.QueryValueEx(k, "RecycleGlass")
        return bool(val) == bool(enabled)
    except OSError:
        return not enabled


def _pulse_existing() -> None:
    """Ask the running instance to play its attention animation."""
    found: list[int] = []

    @w32.ENUMPROC
    def cb(hw, lp):
        b = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hw, b, 256)
        if b.value == CLASS_NAME:
            found.append(hw)
        return True

    try:
        user32.EnumWindows(cb, 0)
        for hw in found:
            user32.PostMessageW(hw, WM_APP_PULSE, 0, 0)
    except Exception:
        pass


def _warmup() -> None:
    """Pre-build fonts, masks, shadows and menu sprites off the UI thread."""
    try:
        # NOTE: never call render.set_scale() here — it mutates module-level
        # metrics that the UI thread is rendering with.
        icon = render.load_app_icon()
        for theme in ("light", "dark"):
            pal = render.palette_for(theme)
            for ht in (0.0, 0.5, 1.0):
                render.render(
                    (render.WINDOW_W, render.WINDOW_H), None, icon,
                    i18n.t("app_name"), i18n.t("sub_empty"),
                    ht > 0.0, pal, 0.78, 0.55,
                    hover_buttons=(i18n.t("btn_open"), i18n.t("btn_empty")),
                    hover_t=ht)
    except Exception:
        pass
    try:
        menu_mod.warmup()
    except Exception:
        pass


_TASKBAR_CREATED = 0


def main() -> int:
    global _TASKBAR_CREATED
    diag.install()
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            pass

    try:
        _TASKBAR_CREATED = int(user32.RegisterWindowMessageW("TaskbarCreated"))
    except Exception:
        _TASKBAR_CREATED = 0

    mutex = kernel32.CreateMutexW(None, False, "RecycleGlassSingleton")
    if not mutex:
        diag.error("mutex creation failed")
        return 1
    if kernel32.GetLastError() == 183:          # ERROR_ALREADY_EXISTS
        diag.info("already running — pulsing the existing widget")
        _pulse_existing()
        return 0

    try:
        app = App()
        if not autostart_matches(app.cfg.autostart):
            diag.info("autostart drifted — rewriting registry")
            apply_autostart(app.cfg.autostart)
        app.create_window()
        diag.info("hwnd=%s strategy=%s", app.hwnd,
                  getattr(app, "strategy", "?"))
        user32.ShowWindow(app.hwnd, 8)  # SW_SHOWNA
        diag.info("entering run loop")
        app.run()
    except Exception:
        diag.exception("fatal error")
        return 1
    diag.info("exited cleanly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
