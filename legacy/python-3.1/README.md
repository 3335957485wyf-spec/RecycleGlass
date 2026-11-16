# RecycleGlass 3.x（Python 原型 · 已归档）

这是 4.0 纯 C 重写之前的 Python 实现，已停止维护，仅作历史参考：

- `recycle_glass/` — PySide 应用（玻璃材质、托盘、拖放、设置的早期形态）
- `run.py` — 启动入口
- `requirements.txt` / `build.spec` / `build.bat` — PyInstaller 打包（约 30 MB）

为什么不继续用：打包 30 MB+、冷启动 1–2 s、常驻内存 150 MB。
4.0 起改用 `src/` 下的纯 C 实现（0.83 MB 单文件、启动即显、空闲 CPU ≈ 0%）。
详见仓库根目录 `CHANGELOG.md` 与 README「版本演进」。
