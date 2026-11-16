"""OLE drop target so the widget can react *while* a file is dragged over it.

DragAcceptFiles only tells us about the drop; to light the card up during the
hover we need IDropTarget (DragEnter / DragOver / DragLeave / Drop).  The
classic WM_DROPFILES path stays available as a fallback when OLE registration
fails for any reason.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

from .w32 import shell32

HRESULT = ctypes.c_long
ULONG = ctypes.c_ulong
DWORD = wintypes.DWORD

CF_HDROP = 15
DVASPECT_CONTENT = 1
TYMED_HGLOBAL = 1
DROPEFFECT_COPY = 1
DROPEFFECT_NONE = 0


class POINTL(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class FORMATETC(ctypes.Structure):
    _fields_ = [
        ("cfFormat", wintypes.WORD),
        ("ptd", ctypes.c_void_p),
        ("dwAspect", DWORD),
        ("lindex", ctypes.c_long),
        ("tymed", DWORD),
    ]


class STGMEDIUM(ctypes.Structure):
    _fields_ = [
        ("tymed", DWORD),
        ("hGlobal", ctypes.c_void_p),
        ("pUnkForRelease", ctypes.c_void_p),
    ]


_FN_QI = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.POINTER(ctypes.c_void_p))
_FN_UL = ctypes.WINFUNCTYPE(ULONG, ctypes.c_void_p)
_FN_ENTER = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.c_void_p,
                               DWORD, POINTL, ctypes.POINTER(DWORD))
_FN_OVER = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, DWORD, POINTL,
                              ctypes.POINTER(DWORD))
_FN_LEAVE = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p)
_FN_DROP = _FN_ENTER


class _Vtbl(ctypes.Structure):
    _fields_ = [
        ("QueryInterface", _FN_QI),
        ("AddRef", _FN_UL),
        ("Release", _FN_UL),
        ("DragEnter", _FN_ENTER),
        ("DragOver", _FN_OVER),
        ("DragLeave", _FN_LEAVE),
        ("Drop", _FN_DROP),
    ]


class _IDropTarget(ctypes.Structure):
    _fields_ = [("lpVtbl", ctypes.POINTER(_Vtbl))]


_FN_GETDATA = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p,
                                 ctypes.POINTER(FORMATETC),
                                 ctypes.POINTER(STGMEDIUM))


def hdrop_paths(hdrop) -> list[str]:
    """Enumerate the file paths inside an HDROP."""
    try:
        n = shell32.DragQueryFileW(hdrop, 0xFFFFFFFF, None, 0)
    except Exception:
        return []
    out: list[str] = []
    for i in range(n):
        buf = ctypes.create_unicode_buffer(1024)
        shell32.DragQueryFileW(hdrop, i, buf, 1024)
        if buf.value:
            out.append(buf.value)
    return out


def _paths_from_dataobject(dataobj) -> list[str]:
    """Pull CF_HDROP out of the IDataObject handed to IDropTarget::Drop."""
    try:
        vt = ctypes.cast(dataobj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        get_data = _FN_GETDATA(vt[3])  # IDataObject::GetData
        fmt = FORMATETC(CF_HDROP, None, DVASPECT_CONTENT, -1, TYMED_HGLOBAL)
        stg = STGMEDIUM()
        hr = get_data(dataobj, ctypes.byref(fmt), ctypes.byref(stg))
        if hr != 0 or not stg.hGlobal:
            return []
        try:
            return hdrop_paths(stg.hGlobal)
        finally:
            try:
                _ole32.ReleaseStgMedium(ctypes.byref(stg))
            except Exception:
                pass
    except Exception:
        return []


try:
    _ole32 = ctypes.WinDLL("ole32")
except OSError:  # pragma: no cover
    _ole32 = None

if _ole32 is not None:
    _ole32.OleInitialize.argtypes = [ctypes.c_void_p]
    _ole32.OleInitialize.restype = HRESULT
    _ole32.RegisterDragDrop.argtypes = [wintypes.HWND, ctypes.c_void_p]
    _ole32.RegisterDragDrop.restype = HRESULT
    _ole32.RevokeDragDrop.argtypes = [wintypes.HWND]
    _ole32.RevokeDragDrop.restype = HRESULT
    _ole32.ReleaseStgMedium.argtypes = [ctypes.POINTER(STGMEDIUM)]


class DropTarget:
    """Minimal IDropTarget implementation."""

    def __init__(self, on_enter, on_leave, on_drop):
        self._on_enter = on_enter
        self._on_leave = on_leave
        self._on_drop = on_drop
        self._refs = 1
        self._registered = False
        self._hwnd = 0

        self._vtbl = _Vtbl(
            _FN_QI(self._query_interface),
            _FN_UL(self._add_ref),
            _FN_UL(self._release),
            _FN_ENTER(self._drag_enter),
            _FN_OVER(self._drag_over),
            _FN_LEAVE(self._drag_leave),
            _FN_DROP(self._drop),
        )
        self._obj = _IDropTarget(ctypes.pointer(self._vtbl))

    # ---- IUnknown ----
    def _query_interface(self, this, riid, ppv):
        if ppv:
            ppv[0] = this
        return 0

    def _add_ref(self, this):
        self._refs += 1
        return self._refs

    def _release(self, this):
        self._refs -= 1
        return self._refs

    # ---- IDropTarget ----
    def _drag_enter(self, this, dataobj, key_state, pt, effect):
        try:
            if effect:
                effect[0] = DROPEFFECT_COPY
        except Exception:
            pass
        try:
            self._on_enter()
        except Exception:
            pass
        return 0

    def _drag_over(self, this, key_state, pt, effect):
        try:
            if effect:
                effect[0] = DROPEFFECT_COPY
        except Exception:
            pass
        return 0

    def _drag_leave(self, this):
        try:
            self._on_leave()
        except Exception:
            pass
        return 0

    def _drop(self, this, dataobj, key_state, pt, effect):
        try:
            if effect:
                effect[0] = DROPEFFECT_COPY
        except Exception:
            pass
        try:
            paths = _paths_from_dataobject(dataobj)
        except Exception:
            paths = []
        try:
            self._on_drop(paths)
        except Exception:
            pass
        return 0

    # ---- registration ----
    def register(self, hwnd) -> bool:
        if _ole32 is None:
            return False
        try:
            _ole32.OleInitialize(None)
            hr = _ole32.RegisterDragDrop(
                hwnd, ctypes.cast(ctypes.byref(self._obj), ctypes.c_void_p))
            self._registered = (hr == 0)
            self._hwnd = hwnd
        except Exception:
            self._registered = False
        return self._registered

    def revoke(self) -> None:
        if not self._registered or _ole32 is None:
            return
        try:
            _ole32.RevokeDragDrop(self._hwnd)
        except Exception:
            pass
        self._registered = False
