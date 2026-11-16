# Baseline: how much does a static card change over 2 s with nothing behind it?
# (The material animates, so two shots are never bit-identical.)
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json
import numpy as np
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
EXE = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22/dist/RecycleGlass-4.11.6.exe"
OUT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22/reference"

subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.6.exe"], capture_output=True)
time.sleep(1.5)
p = subprocess.Popen([EXE])
time.sleep(4.5)

h = u32.FindWindowW("RecycleGlassWidgetC", None)
r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
box = (r.left, r.top, r.right, r.bottom)
a = np.asarray(ImageGrab.grab(bbox=box, all_screens=True).convert("RGB")).astype(int)
time.sleep(2.0)
b = np.asarray(ImageGrab.grab(bbox=box, all_screens=True).convert("RGB")).astype(int)
d = np.abs(a - b).mean(axis=2)
inner = d[45:150, 45:370]
print(f"idle noise 2s apart: whole mean={d.mean():.2f}  interior mean={inner.mean():.2f} "
      f">10={(inner > 10).mean()*100:.2f}%")
