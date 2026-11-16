# Probe: can we capture the DESKTOP WINDOW (wallpaper + icons) without other apps?
# Prints what Progman / WorkerW / SHELLDLL_DefView return through PrintWindow.
import ctypes, ctypes.wintypes as wt, os
from PIL import Image

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
u32.SetProcessDPIAware()

OUT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22\reference"
user32 = u32

EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, b, 256)
    return b.value

def title(h):
    b = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(h, b, 512)
    return b.value

def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom

def find_child(parent, klass):
    found = []
    def cb(h, l):
        if cls(h) == klass:
            found.append(h)
        return True
    u32.EnumChildWindows(parent, EnumWindowsProc(cb), 0)
    return found

tops = []
def on_top(h, l):
    c = cls(h)
    if c in ("Progman", "WorkerW"):
        tops.append((h, c, title(h)))
    return True
u32.EnumWindows(EnumWindowsProc(on_top), 0)

print("desktop hosts:")
for h, c, t in tops:
    defv = find_child(h, "SHELLDLL_DefView")
    print(f"  {c:10s} hwnd={h:08x} rect={rect(h)} title={t!r} DefView={len(defv)}")

def print_window(h, w, hgt, name, flags):
    hdc = u32.GetDC(0)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, hgt)
    g32.SelectObject(mem, bmp)
    g32.BitBlt(mem, 0, 0, w, hgt, hdc, rect(h)[0], rect(h)[1], 0x00CC0020)
    ok = u32.PrintWindow(h, mem, flags)

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                    ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]
    bi = BMIH()
    bi.biSize = ctypes.sizeof(BMIH)
    bi.biWidth = w
    bi.biHeight = -hgt
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * hgt * 4)
    g32.GetDIBits(mem, bmp, 0, hgt, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGBA", (w, hgt), buf, "raw", "BGRA", 0, 1).convert("RGB")
    p = os.path.join(OUT, name + ".png")
    img.save(p)
    small = img.resize((w // 3, hgt // 3), Image.LANCZOS)
    small.save(os.path.join(OUT, name + "-small.png"))
    extrema = img.convert("L").getextrema()
    print(f"  {name}: PrintWindow={ok} size={w}x{hgt} lum={extrema} -> {p}")
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(0, hdc)
    return img

for h, c, t in tops:
    l, tp, r, b = rect(h)
    w, hgt = r - l, b - tp
    if w < 200 or hgt < 200:
        continue
    print(f"\n{c} {h:08x} {w}x{hgt}")
    print_window(h, w, hgt, f"probe-{c}", 2)      # PW_RENDERFULLCONTENT
