# Is the card body opaque?  Dump the layered surface and inspect the alpha.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json
import numpy as np
from PIL import Image

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
ROOT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22"
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")
FRAME = os.path.join(os.environ["TEMP"], "rg_frame.png")
WM_APP = 0x8000
RG_WM_DUMP = WM_APP + 9
RG_WM_TRAY = WM_APP + 1
SWP_NOSIZE, SWP_NOACTIVATE, SWP_NOMOVE = 0x0001, 0x0010, 0x0002

orig = open(CFG, "rb").read()
cfg = json.loads(orig.decode())
cfg["topmost"] = False
json.dump(cfg, open(CFG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)

subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.6.exe"], capture_output=True)
time.sleep(1.5)
p = subprocess.Popen([ROOT + r"\dist\RecycleGlass-4.11.6.exe"])
time.sleep(4.5)

card = u32.FindWindowW("RecycleGlassWidgetC", None)
r = wt.RECT(); u32.GetWindowRect(card, ctypes.byref(r))

note = subprocess.Popen(["notepad.exe"])
time.sleep(1.8)
nh = u32.FindWindowW("Notepad", None)
u32.SetWindowPos(nh, 0, r.left - 20, r.top - 20, 900, 600, SWP_NOSIZE | SWP_NOACTIVATE)
u32.SetWindowPos(card, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
time.sleep(2.0)

if os.path.exists(FRAME):
    os.remove(FRAME)
u32.PostMessageW(card, RG_WM_DUMP, 0, 0)
time.sleep(1.0)
img = Image.open(FRAME).convert("RGBA")
a = np.asarray(img)
print("frame", img.size)
print("alpha in body  x150-330 y55-150:", a[55:150, 150:330, 3].min(), "-", a[55:150, 150:330, 3].max())
print("alpha at top-left corner:", a[3, 3, 3])

subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.6.exe"], capture_output=True)
try:
    p.wait(timeout=8)
except Exception:
    pass
time.sleep(0.5)
open(CFG, "wb").write(orig)
print("config restored")
