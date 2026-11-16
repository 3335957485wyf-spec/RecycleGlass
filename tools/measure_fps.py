# Play a video behind the card and read the app's own fps / cost counters.
import ctypes, ctypes.wintypes as wt, subprocess, time, os, json, hashlib
u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
ROOT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22"
EXE = ROOT + r"\dist\RecycleGlass-4.11.7.exe"
VIDEO = ROOT + r"\reference\rganim.html"
CFG = os.path.join(os.environ["APPDATA"], "RecycleGlass", "config.json")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "RecycleGlass", "recycleglass.log")
EDGE = r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"

def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom

def pids(part):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True).stdout or b""
    res = []
    for line in out.decode("utf-8", "ignore").splitlines():
        p = line.strip().strip('"').split('","')
        if p and part.lower() in p[0].lower() and len(p) > 1:
            try: res.append(int(p[1]))
            except ValueError: pass
    return res

for pid in pids("recycleglass"):
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
subprocess.run(["taskkill", "/F", "/IM", "msedge.exe"], capture_output=True)
time.sleep(1.5)

orig = open(CFG, "rb").read()
sha = hashlib.sha256(orig).hexdigest()
cfg = json.loads(orig.decode()); cfg["topmost"] = True
json.dump(cfg, open(CFG, "w", encoding="utf-8"), indent=2, ensure_ascii=False)

p = subprocess.Popen([EXE], env=dict(os.environ, RG_PROF="1"))
time.sleep(4.0)
card = u32.FindWindowW("RecycleGlassWidgetC", None)
l, t, r, b = rect(card)

subprocess.Popen([EDGE, "--app=file:///" + VIDEO.replace("\\", "/"),
                  "--window-size=900,620",
                  "--window-position=%d,%d" % (max(0, l - 60), max(0, t - 60)),
                  "--autoplay-policy=no-user-gesture-required",
                  "--no-first-run",
                  "--user-data-dir=" + os.path.join(os.environ["TEMP"], "rg-video-profile")])
time.sleep(18.0)
subprocess.run(["taskkill", "/F", "/IM", "msedge.exe"], capture_output=True)
time.sleep(4.0)

subprocess.run(["taskkill", "/F", "/PID", str(p.pid)], capture_output=True)
try: p.wait(timeout=8)
except Exception: pass

log = open(LOG, "rb").read().decode("utf-8", "ignore")
rows = [x for x in log.splitlines() if "live fps=" in x][-10:]
for x in rows:
    print("  ", x.split("  ", 1)[-1].replace("live ", ""))

time.sleep(0.4)
open(CFG, "wb").write(orig)
print("config restored:", hashlib.sha256(open(CFG, "rb").read()).hexdigest() == sha)
