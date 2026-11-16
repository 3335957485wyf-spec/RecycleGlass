# Collects every character the UI can ever draw and subsets MiSans to it.
#
# Safety policy ("zero visual sacrifice"): the character set is the union of
#  - every L"..." literal in src/*.c  (both i18n languages, all units, all
#    hardcoded labels, even diag strings - over-inclusion is free)
#  - the complete printable ASCII range 0x20..0x7E (digits, %, px, MB, GB...)
# A glyph that is missing would silently fall back to another font, so we
# over-collect deliberately; the subset is still tiny next to the 7.7 MB full
# font.
import os, re, sys, subprocess

ROOT = r"C:\Users\hai-3\WorkBuddy\2026-11-15-00-14-22\RecycleGlass"
SRC = os.path.join(ROOT, "src")
DIST = os.path.join(ROOT, "..", "dist")
RES = os.path.join(ROOT, "res")

lit = re.compile(r'L"((?:[^"\\]|\\.)*)"')
chars = set()

for name in os.listdir(SRC):
    if not name.endswith((".c", ".h")):
        continue
    text = open(os.path.join(SRC, name), encoding="utf-8").read()
    for m in lit.finditer(text):
        s = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
        chars.update(s)

# full printable ASCII (readouts, units, numbers, latin UI language)
chars.update(chr(c) for c in range(0x20, 0x7F))
# explicit safety extras
chars.update("·（）、，。：；！？—…％")
chars.discard("\n"); chars.discard("\t"); chars.discard("\r")

out = "".join(sorted(chars))
cjk = sum(1 for c in out if ord(c) > 0x2E80)
txt = os.path.join(RES, "font-chars.txt")
open(txt, "w", encoding="utf-8").write(out)
print(f"unique chars: {len(out)}  (CJK: {cjk})")
print("sample CJK:", "".join(c for c in out if ord(c) > 0x2E80)[:60])

jobs = [
    ("MiSans-Regular.ttf", "misans-sub-regular.ttf"),
    ("MiSans-Bold.ttf", "misans-sub-bold.ttf"),
]
for src_name, dst_name in jobs:
    src = os.path.join(DIST, src_name)
    dst = os.path.join(RES, dst_name)
    cmd = [sys.executable, "-m", "fontTools.subset", src,
           f"--text-file={txt}",
           f"--output-file={dst}",
           "--name-IDs=*", "--glyph-names", "--notdef-outline",
           "--recalc-bounds", "--recalc-average-width",
           "--layout-features=*", "--legacy-kern"]
    subprocess.run(cmd, check=True)
    print(f"{dst_name}: {os.path.getsize(dst)//1024} KB")

# verify: every collected char must exist in BOTH subsets' cmap
from fontTools.ttLib import TTFont
for _, dst_name in jobs:
    f = TTFont(os.path.join(RES, dst_name))
    cmap = f.getBestCmap()
    missing = [c for c in out if ord(c) not in cmap]
    fam = f["name"].getDebugName(1)
    sub = f["name"].getDebugName(2)
    print(f"{dst_name}: family='{fam}' style='{sub}' "
          f"missing={len(missing)} {missing[:10] if missing else ''}")
