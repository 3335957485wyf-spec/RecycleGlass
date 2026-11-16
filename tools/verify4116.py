# 4.11.6: the backdrop source must follow the z-order.
#   topmost = OFF -> desktop only (a window behind must NOT tint the glass)
#   topmost = ON  -> live screen  (a window behind MUST show through)
# The window is pushed to HWND_BOTTOM so it sits behind the card without the
# card needing to be topmost, which is what makes the OFF case measurable.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, hashlib, sys
import numpy as np
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

ROOT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22"
EXE = ROOT + r"\dist\RecycleGlass-4.11.6.exe"
OUT = ROOT + r"\reference"
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "RecycleGlass", "recycleglass.log")

HWND_TOP = 0
SWP_NOSIZE, SWP_NOACTIVATE, SWP_NOMOVE = 0x0001, 0x0010, 0x0002


def find(c): return u32.FindWindowW(c, None)
def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def any_running():
    out = subprocess.run(["tasklist"], capture_output=True).stdout or b""
    return b"recycleglass" in out.lower()


def wait_all_gone(timeout=10.0):
    """The app rewrites config.json while shutting down; a restore that runs
    before the last instance is gone is silently overwritten (this bit us)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not any_running():
            return True
        time.sleep(0.3)
    return False


def kill_proc(p, timeout=8):
    """Waits for OUR process: it writes config.json while shutting down, so the
    restore that follows must not race with it."""
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/PID", str(p.pid)], capture_output=True)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def kill_all():
    for n in ("RecycleGlass.exe", "RecycleGlass-4.11.5.exe", "RecycleGlass-4.11.6.exe"):
        subprocess.run(["taskkill", "/F", "/IM", n], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
    time.sleep(1.2)


orig = open(CFG, "rb").read()
sha = hashlib.sha256(orig).hexdigest()


def run_case(topmost, tag):
    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
    cfg = json.loads(orig.decode("utf-8"))
    cfg["topmost"] = topmost
    json.dump(cfg, open(CFG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)

    before = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    proc = subprocess.Popen([EXE])
    time.sleep(4.0)
    log = open(LOG, "rb").read()[before:].decode("utf-8", "ignore")
    mode = [l for l in log.splitlines() if "backdrop=" in l]
    print(f"[{tag}] {mode[-1] if mode else 'no backdrop line'}")

    card = find("RecycleGlassWidgetC")
    l, t, r, b = rect(card)
    clean = ImageGrab.grab(bbox=(l, t, r, b), all_screens=True)
    clean.save(os.path.join(OUT, f"v4116-{tag}-clean.png"))

    note = subprocess.Popen(["notepad.exe"])
    time.sleep(1.8)
    nh = u32.FindWindowW("Notepad", None)
    u32.SetWindowPos(nh, 0, l - 20, t - 20, 900, 600, SWP_NOSIZE | SWP_NOACTIVATE)
    if not topmost:
        # The card is a normal window here, so lift it above the editor to keep
        # it visible (that is also how a user would see it).
        u32.SetWindowPos(card, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    time.sleep(2.0)

    behind = ImageGrab.grab(bbox=(l, t, r, b), all_screens=True)
    behind.save(os.path.join(OUT, f"v4116-{tag}-behind.png"))

    a = np.asarray(clean.convert("RGB")).astype(int)
    bb = np.asarray(behind.convert("RGB")).astype(int)
    h, w, _ = a.shape
    y0, y1 = int(h * 0.32), int(h * 0.72)
    x0, x1 = int(w * 0.22), int(w * 0.55)
    d = np.abs(a[y0:y1, x0:x1] - bb[y0:y1, x0:x1]).mean(axis=2)
    print(f"[{tag}] glass diff with a window behind: mean={d.mean():.2f} "
          f"max={d.max():.0f} px>10={(d > 10).mean() * 100:.1f}%")

    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "RecycleGlass-4.11.6.exe"], capture_output=True)
    kill_proc(proc)
    time.sleep(0.5)


kill_all()
run_case(False, "desktop")
run_case(True, "screen")
wait_all_gone()
open(CFG, "wb").write(orig)
time.sleep(1.0)
ok = hashlib.sha256(open(CFG, "rb").read()).hexdigest() == sha
if not ok:                       # a late write slipped in: restore again
    wait_all_gone()
    open(CFG, "wb").write(orig)
    ok = hashlib.sha256(open(CFG, "rb").read()).hexdigest() == sha
print("config restored:", ok)
