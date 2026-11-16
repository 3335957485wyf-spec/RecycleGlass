# Dump the exact menu surface (premultiplied BGRA) so text centring can be
# measured in real pixels instead of eyeballed.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, struct, sys
import numpy as np
from PIL import Image

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

ROOT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22"
EXE = sys.argv[1] if len(sys.argv) > 1 else ROOT + r"\dist\RecycleGlass-4.11.2.exe"
OUT = ROOT + r"\reference"
FRAME = os.path.join(os.environ["TEMP"], "rg_menu_frame.bgra")

WM_APP = 0x8000
RG_WM_TRAY = WM_APP + 1
RG_WM_DUMP = WM_APP + 9
WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205


def find(cls):
    return u32.FindWindowW(cls, None)


def rect(h):
    r = wt.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def lp(x, y):
    return ((y << 16) | (x & 0xFFFF)) & 0xFFFFFFFF


subprocess.run(["taskkill", "/F", "/IM", os.path.basename(EXE)], capture_output=True)
time.sleep(0.8)
subprocess.Popen([EXE])
time.sleep(4.0)

cfg = json.load(open(os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json"),
                    encoding="utf-8"))
corner = float(cfg["corner_radius"])

card = find("RecycleGlassWidgetC")
u32.PostMessageW(card, RG_WM_TRAY, 0, WM_RBUTTONUP)
time.sleep(1.0)
menu = find("RecycleGlassMenuC")
ml, mt, mr, mb = rect(menu)
mw, mh = mr - ml, mb - mt
print("menu window", mw, mh)

s = (mw - 8) / 356.0
margin = round(12 * s * 2 + 4)
pad = max(round(corner / 1.618 * s), round(12 * s))
rowh = max(round(44 * s), 30)
sep = max(round(corner / 2.618 * s), 4)
panel = round(308 * s)
x = margin + panel // 2

# main page -> appearance (3rd row: open, empty, sep, appearance)
u32.PostMessageW(menu, WM_LBUTTONUP, 0, lp(x, margin + pad + rowh + sep + rowh + sep + rowh // 2))
time.sleep(1.2)

if os.path.exists(FRAME):
    os.remove(FRAME)
u32.PostMessageW(menu, RG_WM_DUMP, 0, 0)
time.sleep(0.8)
assert os.path.exists(FRAME), "no frame dumped"

raw = open(FRAME, "rb").read()
w, h = struct.unpack("ii", raw[:8])
px = np.frombuffer(raw[8:8 + w * h * 4], np.uint8).reshape(h, w, 4)
b, g, r, a = (px[..., i].astype(np.float32) for i in range(4))
alpha = a / 255.0
# premultiplied -> straight, then composite over the light panel backdrop
rgb = np.zeros((h, w, 3), np.float32)
for i, ch in enumerate((b, g, r)):
    rgb[..., i] = np.where(alpha > 0, ch / np.maximum(alpha, 1e-6), 0)
img = np.clip(rgb * alpha[..., None] + 245.0 * (1 - alpha[..., None]), 0, 255).astype(np.uint8)
Image.fromarray(img).save(os.path.join(OUT, "metric-appearance.png"))
print("frame", w, h, "->", OUT + r"\metric-appearance.png", "scale", s, "pad", pad, "rowh", rowh)

# close the menu, never touching the user's settings
u32.PostMessageW(menu, 0x0010, 0, 0)
time.sleep(0.5)
subprocess.run(["taskkill", "/F", "/IM", os.path.basename(EXE)], capture_output=True)
print("done")
