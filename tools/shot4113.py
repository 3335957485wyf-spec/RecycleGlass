# Final visual check for 4.11.3: captures the appearance page and a 3x zoom of
# a segment row + a menu row, straight from the live window.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, sys
from PIL import ImageGrab, Image

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

ROOT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22"
EXE = sys.argv[1] if len(sys.argv) > 1 else ROOT + r"\dist\RecycleGlass-4.11.3.exe"
OUT = ROOT + r"\reference"
WM_APP = 0x8000
RG_WM_TRAY = WM_APP + 1
WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205

def find(c): return u32.FindWindowW(c, None)
def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom
def lp(x, y): return ((y << 16) | (x & 0xFFFF)) & 0xFFFFFFFF

subprocess.run(["taskkill", "/F", "/IM", os.path.basename(EXE)], capture_output=True)
time.sleep(0.8)
subprocess.Popen([EXE])
time.sleep(4.0)

cfg = json.load(open(os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json"), encoding="utf-8"))
corner = float(cfg["corner_radius"])

card = find("RecycleGlassWidgetC")
cl, ct, cr, cb = rect(card)
ImageGrab.grab(bbox=(cl, ct, cr, cb), all_screens=True).save(os.path.join(OUT, "v4113-card.png"))

u32.PostMessageW(card, RG_WM_TRAY, 0, WM_RBUTTONUP)
time.sleep(1.0)
menu = find("RecycleGlassMenuC")
ml, mt, mr, mb = rect(menu)
ImageGrab.grab(bbox=(ml, mt, mr, mb), all_screens=True).save(os.path.join(OUT, "v4113-menu.png"))

s = (mr - ml - 8) / 356.0
margin = round(12 * s * 2 + 4)
pad = max(round(corner / 1.618 * s), round(12 * s))
rowh = max(round(44 * s), 30)
sep = max(round(corner / 2.618 * s), 4)
x = margin + round(308 * s) // 2

# 3rd row -> appearance
u32.PostMessageW(menu, WM_LBUTTONUP, 0, lp(x, margin + pad + rowh + sep + rowh + sep + rowh // 2))
time.sleep(1.2)
r = rect(menu)
img = ImageGrab.grab(bbox=(r[0], r[1], r[2], r[3]), all_screens=True)
img.save(os.path.join(OUT, "v4113-appearance.png"))

# segments sit right under the "主题" row; crop generously and upscale 3x
c = img.crop((int(img.width * 0.06), int(img.height * 0.22), int(img.width * 0.94), int(img.height * 0.33)))
c.resize((c.width * 3, c.height * 3), Image.LANCZOS).save(os.path.join(OUT, "v4113-seg-zoom.png"))
print("seg zoom", c.size)

# switches row: crop the toggle rows for the knob/label alignment
c2 = img.crop((int(img.width * 0.06), int(img.height * 0.55), int(img.width * 0.94), int(img.height * 0.72)))
c2.resize((c2.width * 3, c2.height * 3), Image.LANCZOS).save(os.path.join(OUT, "v4113-row-zoom.png"))
print("row zoom", c2.size)

u32.PostMessageW(menu, 0x0010, 0, 0)
time.sleep(0.5)
subprocess.run(["taskkill", "/F", "/IM", os.path.basename(EXE)], capture_output=True)
print("done")
