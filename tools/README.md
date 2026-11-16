# tools/ · 开发期脚本（不参与构建）

| 脚本 | 用途 |
| --- | --- |
| `make_font_subset.py` | 扫描 `src/` 全部 `L"..."` 字面量 + 完整 ASCII，用 fontTools 把 MiSans 子集化到 `res/misans-sub-*.ttf`，并校验缺字为 0。新增文案后重跑一次即可。 |
| `render_promo.py` | 4K60 宣传片的程序化渲染器（PIL + numpy 逐帧矢量重绘 + ffmpeg/NVENC 编码，含合成音频）。 |
| `preview_card.py` / `preview_corners.py` | 离线渲染卡片/圆角对比图，不需要桌面。 |
| `check_widget.py` / `check_menu.py` / `check_click.py` | 消息驱动自检：开菜单、翻页、点按、抓帧。 |
| `make_icons.py` / `make_compare.py` | 生成图标变体与视觉对比图。 |

这些脚本只在开发机上使用，编译 exe 不需要它们（见 `Makefile` / `build-mingw.bat`）。
