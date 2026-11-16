# Move the card over the desktop icon column (temporarily) and look at what the
# glass shows.  The window is put back where it was afterwards.
import ctypes, ctypes.wintypes as wt, time, os, json
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
OUT = r"C:/Users/hai-3/WorkBuddy/2026-11-15-00-14-22/reference"

def rect(h):
    r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom

card = u32.FindWindowW("RecycleGlassWidgetC", None)
ox, oy, orr, ob = rect(card)
print("card at", ox, oy)

# wide shot first (desktop icons are in the left/right columns)
ImageGrab.grab(bbox=(0, 0, 900, 700), all_screens=True).save(os.path.join(OUT, "v4115-desktop-left.png"))

u32.SetWindowPos(card, 0, 60, 160, 0, 0, 0x0001 | 0x0004 | 0x0010)  # NOSIZE|NOZORDER|NOACTIVATE
time.sleep(1.6)
nx, ny, nr, nb = rect(card)
print("moved to", nx, ny)
ImageGrab.grab(bbox=(nx - 40, ny - 40, nr + 40, nb + 40), all_screens=True).save(
    os.path.join(OUT, "v4115-card-over-icons.png"))

u32.SetWindowPos(card, 0, ox, oy, 0, 0, 0x0001 | 0x0004 | 0x0010)
time.sleep(1.0)
print("restored to", rect(card))
