"""Thin ctypes wrappers around the Win32 APIs used by RecycleGlass."""
from __future__ import annotations

import ctypes
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)

# ---------------------------------------------------------------------------
# constants
# ---------------------------------------------------------------------------
WS_POPUP = 0x80000000
WS_CHILD = 0x40000000
WS_VISIBLE = 0x10000000
WS_EX_LAYERED = 0x00080000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOPMOST = 0x00000008

GWL_EXSTYLE = -20
GWL_STYLE = -16

SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
SWP_FRAMECHANGED = 0x0020

HWND_TOP = 0
HWND_BOTTOM = 1
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
HWND_MESSAGE = -3

SMTO_NORMAL = 0x0000
SMTO_ABORTIFHUNG = 0x0002

MA_NOACTIVATE = 3
MA_NOACTIVATEANDEAT = 4

ULW_ALPHA = 0x00000002
AC_SRC_OVER = 0
AC_SRC_ALPHA = 1

DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020

WM_QUIT = 0x0012
WM_DESTROY = 0x0002
WM_CLOSE = 0x0010
WM_PAINT = 0x000F
WM_MOVE = 0x0003
WM_SIZE = 0x0005
WM_TIMER = 0x0113
WM_COMMAND = 0x0111
WM_SYSCOMMAND = 0x0112
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEMOVE = 0x0200
WM_MOUSELEAVE = 0x02A3
WM_MOUSEACTIVATE = 0x0021
WM_DROPFILES = 0x0233
WM_SETTINGCHANGE = 0x001A
WM_DISPLAYCHANGE = 0x007E
WM_DPICHANGED = 0x02E0
WM_USER = 0x0400

TME_LEAVE = 0x00000002
TME_NONCLIENT = 0x00000010

MK_LBUTTON = 0x0001
MK_RBUTTON = 0x0002

TPM_RIGHTBUTTON = 0x0002
TPM_RETURNCMD = 0x0100
TPM_NONOTIFY = 0x0080

MF_STRING = 0x0000
MF_SEPARATOR = 0x0800
MF_GRAYED = 0x0001
MF_CHECKED = 0x0008

IDC_ARROW = 32512
COLOR_WINDOW = 5

SM_CXICON = 11
SM_CYICON = 12
SM_CXSMICON = 49
SM_CYSMICON = 50
SM_CXSCREEN = 0
SM_CYSCREEN = 1
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

MONITOR_DEFAULTTONEAREST = 2

SPI_GETDESKWALLPAPER = 0x0073
SPI_SETDESKWALLPAPER = 0x0014
SPIF_UPDATEINIFILE = 0x01
SPIF_SENDCHANGE = 0x02

SHGSI_ICON = 0x00000100
SHGSI_SMALLICON = 0x00000001
SHGSI_LARGEICON = 0x00000000
SIID_RECYCLEBIN = 35
SIID_RECYCLEBINFULL = 36

SHERB_NOCONFIRMATION = 0x00000001
SHERB_NOPROGRESSUI = 0x00000002
SHERB_NOSOUND = 0x00000004

FO_DELETE = 3
FOF_ALLOWUNDO = 0x0040
FOF_NOCONFIRMATION = 0x0010
FOF_SILENT = 0x0004
FOF_NOERRORUI = 0x0400

DWMWA_WINDOW_CORNER_PREFERENCE = 33
DWMWCP_ROUND = 2
DWMWA_SYSTEMBACKDROP_TYPE = 38
DWMWA_USE_IMMERSIVE_DARK_MODE = 20

# shell recycle bin canonical path
RECYCLE_BIN_CLSID = "{645FF040-5081-101B-9F08-00AA002F954E}"

# ---------------------------------------------------------------------------
# structures
# ---------------------------------------------------------------------------


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class SIZE(ctypes.Structure):
    _fields_ = [("cx", wintypes.LONG), ("cy", wintypes.LONG)]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [
        ("BlendOp", ctypes.c_ubyte),
        ("BlendFlags", ctypes.c_ubyte),
        ("SourceConstantAlpha", ctypes.c_ubyte),
        ("AlphaFormat", ctypes.c_ubyte),
    ]


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.UINT),
        ("style", wintypes.UINT),
        ("lpfnWndProc", ctypes.c_void_p),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
        ("hIconSm", wintypes.HICON),
    ]


class TRACKMOUSEEVENT(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("hwndTrack", wintypes.HWND),
        ("dwHoverTime", wintypes.DWORD),
    ]


class SHQUERYRBINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("i64Size", ctypes.c_int64),
        ("i64NumItems", ctypes.c_int64),
    ]


class SHSTOCKICONINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("hIcon", wintypes.HICON),
        ("iSysImageIndex", ctypes.c_int),
        ("iIcon", ctypes.c_int),
        ("szPath", wintypes.WCHAR * 260),
    ]


class ICONINFO(ctypes.Structure):
    _fields_ = [
        ("fIcon", wintypes.BOOL),
        ("xHotspot", wintypes.DWORD),
        ("yHotspot", wintypes.DWORD),
        ("hbmMask", wintypes.HBITMAP),
        ("hbmColor", wintypes.HBITMAP),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


class SHFILEOPSTRUCTW(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("wFunc", wintypes.UINT),
        ("pFrom", wintypes.LPCWSTR),
        ("pTo", wintypes.LPCWSTR),
        ("fFlags", wintypes.WORD),
        ("fAnyOperationsAborted", wintypes.BOOL),
        ("hNameMappings", ctypes.c_void_p),
        ("lpszProgressTitle", wintypes.LPCWSTR),
    ]


# ---------------------------------------------------------------------------
# function prototypes
# ---------------------------------------------------------------------------
WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_longlong,
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
)
ENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

user32.DefWindowProcW.restype = ctypes.c_longlong
user32.DefWindowProcW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.CallWindowProcW.restype = ctypes.c_longlong
user32.GetWindowLongPtrW.restype = ctypes.c_longlong
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetWindowLongPtrW.restype = ctypes.c_longlong
user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_longlong]
user32.SendMessageTimeoutW.restype = ctypes.c_longlong
user32.SetWindowPos.restype = wintypes.BOOL
user32.SetParent.restype = wintypes.HWND
user32.GetParent.restype = wintypes.HWND
user32.IsWindow.restype = wintypes.BOOL
user32.IsWindowVisible.restype = wintypes.BOOL
user32.UpdateLayeredWindow.restype = wintypes.BOOL
user32.UpdateLayeredWindow.argtypes = [
    wintypes.HWND,
    wintypes.HDC,
    ctypes.POINTER(POINT),
    ctypes.POINTER(SIZE),
    wintypes.HDC,
    ctypes.POINTER(POINT),
    wintypes.COLORREF,
    ctypes.POINTER(BLENDFUNCTION),
    wintypes.DWORD,
]
user32.TrackMouseEvent.argtypes = [ctypes.POINTER(TRACKMOUSEEVENT)]
user32.TrackPopupMenu.restype = wintypes.UINT
user32.LoadIconW.restype = wintypes.HICON
user32.LoadImageW.restype = wintypes.HANDLE
user32.DestroyIcon.argtypes = [wintypes.HICON]
user32.GetIconInfo.argtypes = [wintypes.HICON, ctypes.POINTER(ICONINFO)]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.SetLayeredWindowAttributes.restype = wintypes.BOOL

user32.GetDC.restype = wintypes.HDC
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.SetLayeredWindowAttributes.restype = wintypes.BOOL

gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateDIBSection.argtypes = [
    wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD,
]
gdi32.CreateDIBSection.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.GetDIBits.argtypes = [
    wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
    ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT,
]
gdi32.GetObjectW.restype = ctypes.c_int
gdi32.GetObjectW.argtypes = [wintypes.HGDIOBJ, ctypes.c_int, ctypes.c_void_p]

shell32.SHQueryRecycleBinW.restype = ctypes.HRESULT
shell32.SHQueryRecycleBinW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(SHQUERYRBINFO)]
shell32.SHEmptyRecycleBinW.restype = ctypes.HRESULT
shell32.SHGetStockIconInfo.restype = ctypes.HRESULT
shell32.SHGetStockIconInfo.argtypes = [
    ctypes.c_int,
    wintypes.UINT,
    ctypes.POINTER(SHSTOCKICONINFO),
]
shell32.ShellExecuteW.restype = wintypes.HINSTANCE
shell32.ShellExecuteW.argtypes = [
    wintypes.HWND,
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    ctypes.c_int,
]
shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
shell32.DragQueryFileW.restype = wintypes.UINT
# argtypes are REQUIRED: without them the HDROP handle is passed as a
# 32-bit int and truncated on x64, so DragQueryFileW silently reports
# zero files and nothing gets recycled.
shell32.DragQueryFileW.argtypes = [wintypes.HANDLE, wintypes.UINT,
                                   wintypes.LPWSTR, wintypes.UINT]
shell32.DragFinish.argtypes = [wintypes.HANDLE]
shell32.SHFileOperationW.restype = ctypes.c_int
shell32.SHFileOperationW.argtypes = [ctypes.POINTER(SHFILEOPSTRUCTW)]
shell32.SHChangeNotify.restype = None

kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]

dwmapi.DwmSetWindowAttribute.restype = ctypes.HRESULT
dwmapi.DwmSetWindowAttribute.argtypes = [
    wintypes.HWND,
    wintypes.DWORD,
    ctypes.c_void_p,
    wintypes.DWORD,
]


def get_last_error():
    return ctypes.get_last_error()

# --- additional prototypes ---
user32.GetCursorPos.argtypes = [ctypes.POINTER(POINT)]
user32.CreatePopupMenu.restype = wintypes.HMENU
user32.DestroyMenu.argtypes = [wintypes.HMENU]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.SetTimer.restype = wintypes.UINT
user32.SetTimer.argtypes = [wintypes.HWND, ctypes.c_size_t, wintypes.UINT, ctypes.c_void_p]
user32.KillTimer.argtypes = [wintypes.HWND, ctypes.c_size_t]
user32.GetMessageW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.UINT]
user32.GetMessageW.restype = ctypes.c_int
user32.PeekMessageW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
user32.PeekMessageW.restype = wintypes.BOOL
user32.TranslateMessage.argtypes = [ctypes.c_void_p]
user32.DispatchMessageW.argtypes = [ctypes.c_void_p]
user32.LoadCursorW.restype = wintypes.HANDLE
user32.CreateWindowExW.restype = wintypes.HWND
user32.RegisterClassExW.restype = wintypes.ATOM
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SystemParametersInfoW.restype = wintypes.BOOL
user32.SystemParametersInfoW.argtypes = [wintypes.UINT, wintypes.UINT, ctypes.c_void_p, wintypes.UINT]
user32.GetSystemMetrics.argtypes = [ctypes.c_int]
user32.GetSystemMetrics.restype = ctypes.c_int
user32.FindWindowW.restype = wintypes.HWND
user32.FindWindowExW.restype = wintypes.HWND
user32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
user32.AppendMenuW.argtypes = [wintypes.HMENU, wintypes.UINT, ctypes.c_size_t, wintypes.LPCWSTR]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]

kernel32.CreateMutexW.restype = wintypes.HANDLE
kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]

kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]

user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p,
]
user32.RegisterClassExW.restype = wintypes.ATOM
user32.RegisterClassExW.argtypes = [ctypes.c_void_p]
user32.LoadCursorW.restype = wintypes.HANDLE
user32.LoadCursorW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]

user32.SetCapture.restype = wintypes.HWND
user32.SetCapture.argtypes = [wintypes.HWND]
user32.ReleaseCapture.restype = wintypes.BOOL
user32.GetAsyncKeyState.restype = wintypes.SHORT
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_longlong
user32.LoadImageW.restype = wintypes.HANDLE
user32.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT, ctypes.c_int, ctypes.c_int, wintypes.UINT]

WM_SETICON = 0x0080
ICON_SMALL = 0
ICON_BIG = 1
VK_LBUTTON = 0x01

# --- system tray ---
class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("hWnd", wintypes.HWND),
        ("uID", wintypes.UINT),
        ("uFlags", wintypes.UINT),
        ("uCallbackMessage", wintypes.UINT),
        ("hIcon", wintypes.HICON),
        ("szTip", wintypes.WCHAR * 128),
        ("dwState", wintypes.DWORD),
        ("dwStateMask", wintypes.DWORD),
        ("szInfo", wintypes.WCHAR * 256),
        ("uTimeoutOrVersion", wintypes.UINT),
        ("szInfoTitle", wintypes.WCHAR * 64),
        ("dwInfoFlags", wintypes.DWORD),
        ("guidItem", ctypes.c_byte * 16),
        ("hBalloonIcon", wintypes.HICON),
    ]

shell32.Shell_NotifyIconW.restype = wintypes.BOOL
shell32.Shell_NotifyIconW.argtypes = [wintypes.DWORD, ctypes.POINTER(NOTIFYICONDATAW)]

NIM_ADD = 0x00000000
NIM_MODIFY = 0x00000001
NIM_DELETE = 0x00000002
NIF_MESSAGE = 0x00000001
NIF_ICON = 0x00000002
NIF_TIP = 0x00000004
NIF_REALTIME = 0x00000010
WM_APP_TRAY = 0x8000 + 1  # WM_APP + 1
WM_LBUTTONUP_TRAY = 0x0202
WM_RBUTTONUP_TRAY = 0x0205
WM_LBUTTONDBLCLK_TRAY = 0x0203

user32.GetCapture.restype = wintypes.HWND

user32.GetForegroundWindow.restype = wintypes.HWND

kernel32.GetCurrentProcessId.restype = wintypes.DWORD

# --- low-level mouse hook ---
user32.SetWindowsHookExW.restype = wintypes.HHOOK
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, ctypes.c_void_p, wintypes.HINSTANCE, wintypes.DWORD]
user32.UnhookWindowsHookEx.restype = wintypes.BOOL
user32.UnhookWindowsHookEx.argtypes = [wintypes.HHOOK]
user32.CallNextHookEx.restype = ctypes.c_long
user32.CallNextHookEx.argtypes = [wintypes.HHOOK, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]


class CHANGEFILTERSTRUCT(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("ExtStatus", wintypes.DWORD)]


def allow_dropped_files(hwnd) -> None:
    # Let WM_DROPFILES through the UIPI filter.  When the widget runs at a
    # different integrity level than Explorer the shell drops the message
    # and dragging files onto the card appears to do nothing.
    try:
        user32.ChangeWindowMessageFilterEx.restype = wintypes.BOOL
        user32.ChangeWindowMessageFilterEx.argtypes = [
            wintypes.HWND, wintypes.UINT, wintypes.DWORD, ctypes.c_void_p
        ]
        cfs = CHANGEFILTERSTRUCT()
        cfs.cbSize = ctypes.sizeof(cfs)
        for msg in (0x0233, 0x0049):  # WM_DROPFILES, WM_COPYGLOBALDATA
            user32.ChangeWindowMessageFilterEx(hwnd, msg, 1, ctypes.byref(cfs))
    except Exception:
        pass


user32.GetDpiForWindow.restype = wintypes.UINT
user32.GetDpiForWindow.argtypes = [wintypes.HWND]


user32.GetDpiForSystem.restype = wintypes.UINT
user32.GetDpiForSystem.argtypes = []



class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [
        ("hdc", ctypes.c_void_p), ("fErase", wintypes.BOOL),
        ("rcPaint", RECT), ("fRestore", wintypes.BOOL),
        ("fIncUpdate", wintypes.BOOL), ("rgbReserved", ctypes.c_byte * 32),
    ]


user32.BeginPaint.restype = ctypes.c_void_p
user32.BeginPaint.argtypes = [wintypes.HWND, ctypes.POINTER(PAINTSTRUCT)]
user32.EndPaint.argtypes = [wintypes.HWND, ctypes.POINTER(PAINTSTRUCT)]
user32.ValidateRect.argtypes = [wintypes.HWND, ctypes.c_void_p]



user32.WindowFromPoint.restype = wintypes.HWND
user32.WindowFromPoint.argtypes = [POINT]



user32.SetParent.restype = wintypes.HWND
user32.SetParent.argtypes = [wintypes.HWND, wintypes.HWND]
user32.FindWindowExW.restype = wintypes.HWND
user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.SendMessageTimeoutW.restype = ctypes.c_long  # LRESULT
user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                        wintypes.LPARAM, wintypes.UINT,
                                        wintypes.UINT, ctypes.POINTER(ctypes.c_void_p)]


# ---------------------------------------------------------------------------
# monitors — the widget must work on any screen of a multi-monitor desktop
# ---------------------------------------------------------------------------
class MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("rcMonitor", RECT),
        ("rcWork", RECT),
        ("dwFlags", wintypes.DWORD),
    ]


user32.MonitorFromWindow.restype = wintypes.HANDLE
user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
user32.MonitorFromPoint.restype = wintypes.HANDLE
user32.MonitorFromPoint.argtypes = [POINT, wintypes.DWORD]
user32.GetMonitorInfoW.restype = wintypes.BOOL
user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MONITORINFO)]
user32.RegisterWindowMessageW.restype = wintypes.UINT
user32.RegisterWindowMessageW.argtypes = [wintypes.LPCWSTR]
user32.InvalidateRect.argtypes = [wintypes.HWND, ctypes.c_void_p, wintypes.BOOL]
