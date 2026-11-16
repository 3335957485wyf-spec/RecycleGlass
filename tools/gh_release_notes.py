# Rewrite the v4.11.5 release notes: the auto-generated body pointed at
# v4.11.4, a tag that no longer exists (its patch releases were folded in).
import subprocess, json, urllib.request

REPO = "3335957485wyf-spec/RecycleGlass"
TAG = "v4.11.5"

BODY = """把回收站装进一块玻璃 · **4.11.5**

这一版合并了原 4.11.2 / 4.11.3 / 4.11.4 三个补丁版（它们不再单独发布），
并修好了玻璃里丢失的桌面图标。

### 玻璃：只玻璃桌面，而且包含图标
- 卡片改为**让桌面窗口自己渲染**（`PrintWindow` + `PW_RENDERFULLCONTENT`）：
  壁纸与桌面图标一起进入玻璃，其他软件窗口天然不在其中
- 逐像素验证：同位置有 / 无窗口时，玻璃内部差异 **0.00**
- 按事件抓取而非每帧：壁纸变化 / 图标数量变化 / 跨屏 / 20s 兜底，整屏一次约 30ms
- 右键菜单仍取屏幕——它覆盖在“用户正在看的东西”之上，显示壁纸会在窗口上开一个洞

### 外观
- 圆角滑块的轨道与分段控件共用同一内容盒，圆环外沿落在端点：左右端偏差 **≤1px**
  （修复前右侧约 18px）

### 文字
- 全部文字改「墨迹居中」：GDI+ 行盒中心比汉字视觉中心低 0.11em，原来每个标签都偏高
  1.5–2px，现在复测偏差 **0.00px**
- 清空确认页长文案按可用宽度自适应分行，不再被裁切；卡片副标题超宽时单行省略

---

**下载**：`RecycleGlass.exe` — 0.95 MB，单文件免安装，自带 MiSans 子集
**SHA-256**：`20b9724a963def2e9464df8584696a6ff4ee9b9a5b4474444ed283e90cf3cf84`
**完整更新日志**：[CHANGELOG.md](https://github.com/3335957485wyf-spec/RecycleGlass/blob/main/CHANGELOG.md)
**对比**：[v4.11.1...v4.11.5](https://github.com/3335957485wyf-spec/RecycleGlass/compare/v4.11.1...v4.11.5)
"""


def token():
    out = subprocess.run(["git", "credential", "fill"],
                         input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise SystemExit("no token")


T = token()


def api(path, method="GET", payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request("https://api.github.com" + path, data=data, method=method,
                                 headers={"Authorization": "Bearer " + T,
                                          "Accept": "application/vnd.github+json",
                                          "Content-Type": "application/json",
                                          "User-Agent": "rg-release-tool"})
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            body = r.read().decode("utf-8")
            return r.status, (json.loads(body) if body.strip() else None)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "ignore")[:300]


st, rel = api(f"/repos/{REPO}/releases/tags/{TAG}")
if st != 200:
    raise SystemExit(f"release lookup failed: {st} {rel}")

st, out = api(f"/repos/{REPO}/releases/{rel['id']}", "PATCH", {"body": BODY})
print("patch", st)
if st == 200:
    print("body length", len(out["body"]))
