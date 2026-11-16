# 4.11.4 checks, in one launch:
#  1) the card's glass must ignore windows behind it (wallpaper only)
#  2) the corner slider's track must share the segmented control's edges
# Temporarily flips "topmost" so a test window can sit BEHIND the card, then
# restores the user's config byte for byte.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, struct, hashlib, sys
import numpy as np
from PIL import ImageGrab, Image

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

ROOT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22"
EXE = sys.argv[1] if len(sys.argv) > 1 else ROOT + r"\dist\RecycleGlass-4.11.5.exe"
OUT = ROOT + r"\reference"
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "RecycleGlass", "recycleglass.log")
FRAME = os.path.join(os.environ["TEMP"], "rg_menu_frame.bgra")

WM_APP = 0x8000
RG_WM_TRAY = WM_APP + 1
RG_WM_DUMP = WM_APP + 9
WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205
SWP_NOZORDER, SWP_NOACTIVATE = 0x0004, 0x0010

def find(c): return u32.FindWindowW(c, None)
def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom
def lp(x, y): return ((y << 16) | (x & 0xFFFF)) & 0xFFFFFFFF

def kill_all():
    subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass.exe"], capture_output=True)
    for v in ("4.11.0", "4.11.1", "4.11.2", "4.11.3", "4.11.4", "4.11.5", "test"):
        subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-%s.exe" % v], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
    time.sleep(1.0)

os.makedirs(OUT, exist_ok=True)
orig = open(CFG, "rb").read()
orig_sha = hashlib.sha256(orig).hexdigest()

kill_all()
assert find("RecycleGlassWidgetC") == 0, "an old instance still holds the window"

cfg = json.loads(orig.decode("utf-8"))
corner = float(cfg["corner_radius"])
cfg["topmost"] = True
json.dump(cfg, open(CFG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)

log_before = os.path.getsize(LOG) if os.path.exists(LOG) else 0
subprocess.Popen([EXE])
time.sleep(4.0)
log = open(LOG, "rb").read()[log_before:]
assert b"start v4.11.5" in log, "not the new build: " + log[-300:].decode("utf-8", "ignore")
print("log:", [l for l in log.decode("utf-8", "ignore").splitlines() if "start" in l][-1])

card = find("RecycleGlassWidgetC")
cl, ct, cr, cb = rect(card)
print("card", cl, ct, cr - cl, cb - ct, "corner", corner)

base = ImageGrab.grab(bbox=(cl, ct, cr, cb), all_screens=True)
base.save(os.path.join(OUT, "v4114-card-clean.png"))

note = subprocess.Popen(["notepad.exe"])
time.sleep(1.8)
nh = u32.FindWindowW("Notepad", None)
u32.SetWindowPos(nh, 0, cl - 30, ct - 30, 900, 600, SWP_NOZORDER | SWP_NOACTIVATE)
u32.SetForegroundWindow(nh)
time.sleep(2.0)
behind = ImageGrab.grab(bbox=(cl, ct, cr, cb), all_screens=True)
behind.save(os.path.join(OUT, "v4114-card-window-behind.png"))

a = np.asarray(base.convert("RGB")).astype(int)
b = np.asarray(behind.convert("RGB")).astype(int)
h, w, _ = a.shape
y0, y1 = int(h * 0.22), int(h * 0.78)
x0, x1 = int(w * 0.06), int(w * 0.62)
d = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).mean(axis=2)
print("glass interior diff with/without a window behind: mean=%.2f max=%.0f px>12=%.2f%%"
      % (d.mean(), d.max(), (d > 12).mean() * 100))

subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
time.sleep(0.8)

u32.PostMessageW(card, RG_WM_TRAY, 0, WM_RBUTTONUP)
time.sleep(1.0)
menu = find("RecycleGlassMenuC")
ml, mt, mr, mb = rect(menu)
mw = mr - ml
s = (mw - 8) / 356.0
margin = round(12 * s * 2 + 4)
pad = max(round(corner / 1.618 * s), round(12 * s))
rowh = max(round(44 * s), 30)
sep = max(round(corner / 2.618 * s), 4)
x = margin + round(308 * s) // 2

u32.PostMessageW(menu, WM_LBUTTONUP, 0, lp(x, margin + pad + rowh + sep + rowh + sep + rowh // 2))
time.sleep(1.2)

if os.path.exists(FRAME):
    os.remove(FRAME)
u32.PostMessageW(menu, RG_WM_DUMP, 0, 0)
time.sleep(0.8)
raw = open(FRAME, "rb").read()
w, hh = struct.unpack("ii", raw[:8])
px = np.frombuffer(raw[8:8 + w * hh * 4], np.uint8).reshape(hh, w, 4)
bb, gg, rr, aa = (px[..., i].astype(np.float32) for i in range(4))
alpha = np.maximum(aa / 255.0, 1e-6)
rgb = np.zeros((hh, w, 3), np.float32)
for i, ch in enumerate((rr, gg, bb)):
    rgb[..., i] = np.where(aa > 0, ch / alpha, 0)
img = np.clip(rgb * alpha[..., None] + 245.0 * (1 - alpha[..., None]), 0, 255).astype(np.uint8)
Image.fromarray(img).save(os.path.join(OUT, "v4114-appearance.png"))

lum = img.astype(float).mean(axis=2)

def runs(mask, minlen=10):
    out, s2 = [], None
    for i, v in enumerate(mask):
        if v and s2 is None: s2 = i
        elif not v and s2 is not None:
            if i - s2 >= minlen: out.append((s2, i - 1))
            s2 = None
    if s2 is not None and len(mask) - s2 >= minlen: out.append((s2, len(mask) - 1))
    return out

best = (0, 0, 0); by = 0
for y in range(120, hh - 120):
    for r0 in runs(lum[y] < 240, 150):
        if r0[1] - r0[0] > best[0]:
            best = (r0[1] - r0[0], r0[0], r0[1]); by = y
print("segmented track row %d: x %d..%d" % (by, best[1], best[2]))

r_, g_, b_ = img[..., 0].astype(float), img[..., 1].astype(float), img[..., 2].astype(float)
accent = (np.abs(b_ - r_) > 30) & (b_ > 120)
bs = (0, 0, 0); sy = 0
for y in range(300, hh - 60):
    for r0 in runs(accent[y], 100):
        if r0[1] - r0[0] > bs[0]:
            bs = (r0[1] - r0[0], r0[0], r0[1]); sy = y
print("slider row %d: accent x %d..%d" % (sy, bs[1], bs[2]))
print("left edge delta %+d px | right edge delta %+d px" % (bs[1] - best[1], bs[2] - best[2]))

u32.PostMessageW(menu, 0x0010, 0, 0)
time.sleep(0.5)
kill_all()
open(CFG, "wb").write(orig)
print("config restored:", hashlib.sha256(open(CFG, "rb").read()).hexdigest() == orig_sha)
print("done")
