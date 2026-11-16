"""Diagnostics: a rotating log, crash reports and last-resort guards.

A desktop gadget has no console, so every failure used to be invisible: the
process simply vanished.  Everything that can go wrong now lands in
``%LOCALAPPDATA%\\RecycleGlass\\logs\\recycleglass.log`` (rotated, 3 x 512 KB)
and unhandled exceptions are recorded with a full traceback before the process
exits.
"""
from __future__ import annotations

import faulthandler
import logging
import os
import sys
import threading
import traceback
from logging.handlers import RotatingFileHandler
from pathlib import Path

APP_NAME = "RecycleGlass"
_LOGGER: logging.Logger | None = None
_LOG_FILE: Path | None = None
_installed = False


def data_dir() -> Path:
    """Per-user writable folder (LocalAppData, falling back to AppData)."""
    base = (os.environ.get("LOCALAPPDATA")
            or os.environ.get("APPDATA")
            or str(Path.home()))
    d = Path(base) / APP_NAME
    try:
        d.mkdir(parents=True, exist_ok=True)
    except OSError:
        d = Path(os.environ.get("TEMP", "."))
    return d


def log_path() -> Path:
    d = data_dir() / "logs"
    try:
        d.mkdir(parents=True, exist_ok=True)
    except OSError:
        return data_dir() / "recycleglass.log"
    return d / "recycleglass.log"


def logger() -> logging.Logger:
    global _LOGGER, _LOG_FILE
    if _LOGGER is not None:
        return _LOGGER
    lg = logging.getLogger(APP_NAME)
    lg.setLevel(logging.DEBUG)
    lg.propagate = False
    try:
        path = log_path()
        h = RotatingFileHandler(path, maxBytes=512 * 1024, backupCount=3,
                                encoding="utf-8")
        h.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)-7s %(message)s", "%Y-%m-%d %H:%M:%S"))
        lg.addHandler(h)
        _LOG_FILE = path
    except OSError:
        pass
    _LOGGER = lg
    return lg


def log(msg: str, level: int = logging.INFO, *args) -> None:
    try:
        logger().log(level, msg, *args)
    except Exception:
        pass


def info(msg: str, *args) -> None:
    log(msg, logging.INFO, *args)


def warn(msg: str, *args) -> None:
    log(msg, logging.WARNING, *args)


def error(msg: str, *args) -> None:
    log(msg, logging.ERROR, *args)


def exception(msg: str) -> None:
    try:
        logger().error("%s\n%s", msg, traceback.format_exc())
    except Exception:
        pass


def install() -> None:
    """Route Python crashes (main thread, threads, hard faults) into the log."""
    global _installed
    if _installed:
        return
    _installed = True
    logger()

    def _hook(exc_type, exc, tb):
        if issubclass(exc_type, KeyboardInterrupt):
            sys.__excepthook__(exc_type, exc, tb)
            return
        try:
            logger().critical("unhandled exception",
                              exc_info=(exc_type, exc, tb))
        except Exception:
            pass
        # never show a Python traceback dialog to the user
        try:
            sys.__excepthook__(exc_type, exc, tb)
        except Exception:
            pass

    sys.excepthook = _hook

    def _thread_hook(args):
        try:
            logger().critical("unhandled exception in thread %s",
                              getattr(args.thread, "name", "?"),
                              exc_info=(args.exc_type, args.exc_value,
                                        args.exc_traceback))
        except Exception:
            pass

    try:
        threading.excepthook = _thread_hook
    except Exception:
        pass

    # hard crashes (access violation inside a ctypes call) leave no traceback;
    # faulthandler writes the native stack to its own small file
    try:
        fh = open(data_dir() / "fault.log", "a", encoding="utf-8")
        faulthandler.enable(file=fh, all_threads=True)
    except Exception:
        pass

    info("=" * 60)
    try:
        from . import __version__ as _ver
    except Exception:
        _ver = "?"
    info("start pid=%s version=%s frozen=%s python=%s", os.getpid(), _ver,
         getattr(sys, "frozen", False), sys.version.split()[0])
    info("log file: %s", _LOG_FILE)
