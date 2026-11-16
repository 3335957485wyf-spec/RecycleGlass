# Verify the 4.11.2 fix for the clipped "永久删除 N 项 · X MB" line.
# Drives the REAL app with messages (no cursor), grabs native pixels, and never
# touches the user's config or the recycle bin contents.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

ROOT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22"
EXE = ROOT + r"\dist\RecycleGlass-4.11.2.exe"
OUT = ROOT + r"\reference"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "RecycleGlass", "recycleglass.log")
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")

WM_APP = 0x8000
RG_WM_TRAY = WM_APP + 1
WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205

os.makedirs(OUT, exist_ok=True)


def find(cls):
    return u32.FindWindowW(cls, None)


def rect(h):
    r = wt.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def grab(box, name):
    img = ImageGrab.grab(bbox=box, all_screens=True)
    p = os.path.join(OUT, name + ".png")
    img.save(p)
    print("shot", name, img.size)
    return p


def lp(x, y):
    return ((y << 16) | (x & 0xFFFF)) & 0xFFFFFFFF


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


# ---- clean slate: the app is single-instance, an old one would shadow this run
subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.1.exe"], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.2.exe"], capture_output=True)
time.sleep(0.8)

cfg = json.load(open(CFG, encoding="utf-8"))
corner = float(cfg.get("corner_radius", 52.0))
print("config corner", corner, "theme", cfg.get("theme"), "accent", cfg.get("accent"))

before = log_size()
subprocess.Popen([EXE])
time.sleep(4.0)

raw = open(LOG, "rb").read()[before:]
assert b"start v4.11.2" in raw, "4.11.2 did NOT start: " + raw[-400:].decode("utf-8", "ignore")
print("log:", [l for l in raw.decode("utf-8", "ignore").splitlines() if "start" in l])

card = find("RecycleGlassWidgetC")
print("card", hex(card or 0))
cl, ct, cr, cb = rect(card)
grab((cl, ct, cr, cb), "fix-card")

# open the menu (tray right-click equivalent)
u32.PostMessageW(card, RG_WM_TRAY, 0, WM_RBUTTONUP)
time.sleep(1.0)
menu = find("RecycleGlassMenuC")
ml, mt, mr, mb = rect(menu)
mw, mh = mr - ml, mb - mt
print("menu main", mw, mh)
grab((ml, mt, mr, mb), "fix-menu-main")

s = (mw - 8) / 356.0
margin = round(12 * s * 2 + 4)
pad = max(round(corner / 1.618 * s), round(12 * s))
rowh = max(round(44 * s), 30)
panel = round(308 * s)
x = margin + panel // 2

# "清空回收站" is the second row of the main page -> its own page with the prompt
y = margin + pad + rowh + rowh // 2
u32.PostMessageW(menu, WM_LBUTTONUP, 0, lp(x, y))
time.sleep(1.2)

r = rect(menu)
print("delete page", r[2] - r[0], r[3] - r[1])
img = ImageGrab.grab(bbox=(r[0], r[1], r[2], r[3]), all_screens=True)
img.save(os.path.join(OUT, "fix-delete-page.png"))
img.resize((img.width * 2, img.height * 2)).save(os.path.join(OUT, "fix-delete-zoom.png"))
print("shot fix-delete-page", img.size)

# zoom the prompt plate only (panel area, above the buttons)
pad_px = margin
plate = img.crop((pad_px, int(img.height * 0.28), img.width - pad_px, int(img.height * 0.72)))
plate.resize((plate.width * 3, plate.height * 3)).save(os.path.join(OUT, "fix-prompt-zoom.png"))
print("shot fix-prompt-zoom", plate.size)

# leave the page with the back row, then close the menu; never touch 永久删除
u32.PostMessageW(menu, WM_LBUTTONUP, 0, lp(x, margin + pad + 25))
time.sleep(0.7)
u32.PostMessageW(menu, 0x0010, 0, 0)   # WM_CLOSE
time.sleep(0.6)
print("menu closed:", find("RecycleGlassMenuC") == 0)

subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.2.exe"], capture_output=True)
print("done")
