"""Recycle Bin shell operations: query / empty / open / recycle dropped files."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path

from . import diag
from . import i18n
from . import w32
from .w32 import shell32, user32, SHQUERYRBINFO, SHSTOCKICONINFO, SHFILEOPSTRUCTW
from .desktop import icon_to_bgra

RECYCLE_CLSID_PATH = f"shell:::{w32.RECYCLE_BIN_CLSID}"


def query() -> tuple[int, int]:
    """Return (item_count, size_bytes)."""
    info = SHQUERYRBINFO()
    info.cbSize = ctypes.sizeof(info)
    hr = shell32.SHQueryRecycleBinW(None, ctypes.byref(info))
    if hr != 0:
        return 0, 0
    return int(info.i64NumItems), int(info.i64Size)


def empty(confirm: bool = True) -> tuple[bool, int, int]:
    """Empty the recycle bin; returns (ok, remaining_count, remaining_size).

    ``SHEmptyRecycleBin`` cannot be trusted to report success: it returns
    S_FALSE / E_UNEXPECTED on perfectly successful runs (reported by many apps,
    reproducible here with a single item — the file was deleted but the call
    returned an error).  So the result is verified by querying the bin again:
    empty means success, fewer items means partial, unchanged means failure.
    """
    flags = w32.SHERB_NOPROGRESSUI | w32.SHERB_NOSOUND
    if not confirm:
        flags |= w32.SHERB_NOCONFIRMATION
    hr = int(shell32.SHEmptyRecycleBinW(None, None, flags))
    # no query *before* the call: with 19k items that alone costs 15 s and
    # would delay the deletion the user just asked for
    after, size = query()
    ok = (after == 0)
    diag.info("SHEmptyRecycleBin hr=0x%08X remaining=%s", hr & 0xFFFFFFFF, after)
    return ok, after, size


def open_bin() -> None:
    shell32.ShellExecuteW(None, "open", RECYCLE_CLSID_PATH, None, None, 1)


def recycle_paths(paths: list[str]) -> tuple[bool, str]:
    """Send a list of filesystem paths to the recycle bin.

    Returns (ok, message). Non-existent paths are skipped.
    """
    existing = [p for p in paths if Path(p).exists()]
    if not existing:
        return False, i18n.t("toast_nothing")

    # SHFileOperationW expects a double-null-terminated, null-separated list.
    joined = "\0".join(existing) + "\0\0"
    buf = ctypes.create_unicode_buffer(joined)
    op = SHFILEOPSTRUCTW()
    op.hwnd = 0
    op.wFunc = w32.FO_DELETE
    op.pFrom = ctypes.cast(buf, wintypes.LPCWSTR)
    op.pTo = None
    op.fFlags = w32.FOF_ALLOWUNDO | w32.FOF_NOCONFIRMATION | w32.FOF_SILENT | w32.FOF_NOERRORUI
    op.fAnyOperationsAborted = False
    op.hNameMappings = None
    op.lpszProgressTitle = None

    rc = shell32.SHFileOperationW(ctypes.byref(op))

    if rc == 0 and not op.fAnyOperationsAborted:
        return True, i18n.t("toast_recycled", n=len(existing))

    # Cloud-backed folders (OneDrive, etc.) can report DE_FILENOTFOUND after
    # the item has already been moved to the bin, so the return code alone is
    # not trustworthy — verify against the filesystem instead.
    gone = [p for p in existing if not Path(p).exists()]
    if len(gone) == len(existing):
        return True, i18n.t("toast_recycled", n=len(existing))

    remaining = len(existing) - len(gone)
    if gone:
        return False, i18n.t("toast_partial", n=remaining)
    return False, f"{i18n.t('toast_failed')} (code {rc})"


def stock_icon_pixels(full: bool) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    """Return the system Recycle Bin icon as (w, h, rgba pixels)."""
    sii = SHSTOCKICONINFO()
    sii.cbSize = ctypes.sizeof(sii)
    sid = w32.SIID_RECYCLEBINFULL if full else w32.SIID_RECYCLEBIN
    hr = shell32.SHGetStockIconInfo(sid, w32.SHGSI_ICON | w32.SHGSI_LARGEICON, ctypes.byref(sii))
    if hr != 0 or not sii.hIcon:
        return 0, 0, []
    try:
        return icon_to_bgra(sii.hIcon)
    finally:
        user32.DestroyIcon(sii.hIcon)


def format_size(n: int) -> str:
    if n <= 0:
        return "0 B"
    units = ["B", "KB", "MB", "GB", "TB"]
    v = float(n)
    for u in units:
        if v < 1024 or u == units[-1]:
            if u == "B":
                return f"{int(v)} {u}"
            return f"{v:.2f} {u}"
        v /= 1024
    return f"{v:.2f} TB"


def format_count(n: int) -> str:
    if not n:
        return i18n.t("sub_empty")
    return f"{n} 项" if i18n.is_cjk() else f"{n} item" + ("s" if n != 1 else "")
