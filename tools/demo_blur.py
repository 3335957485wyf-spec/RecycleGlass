# One demo shot at a "normal" frosted setting so the icons read as a soft wash.
# The user's config is restored byte for byte afterwards.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, hashlib, sys
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
ROOT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22"
EXE = ROOT + r"\dist\RecycleGlass-4.11.5.exe"
OUT = ROOT + r"\reference"
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")

def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom

orig = open(CFG, "rb").read()
sha = hashlib.sha256(orig).hexdigest()

subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.5.exe"], capture_output=True)
time.sleep(1.0)

cfg = json.loads(orig.decode("utf-8"))
cfg.update({"opacity": 0.92, "material": 0.55, "blur_radius": 26.0})
cfg["x"], cfg["y"] = 60, 150
json.dump(cfg, open(CFG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)

subprocess.Popen([EXE])
time.sleep(4.5)
card = u32.FindWindowW("RecycleGlassWidgetC", None)
l, t, r, b = rect(card)
ImageGrab.grab(bbox=(l - 30, t - 30, r + 30, b + 30), all_screens=True).save(
    os.path.join(OUT, "v4115-frost-demo.png"))
print("shot", r - l, b - t)

subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.5.exe"], capture_output=True)
time.sleep(0.8)
open(CFG, "wb").write(orig)
print("restored:", hashlib.sha256(open(CFG, "rb").read()).hexdigest() == sha)
