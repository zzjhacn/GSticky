# GSticky Windows 版验证记录

> **唯一权威验证文档**。只保留适用口径、设计依据、可复现的验证方式与已知限制；一次性探测脚本、过程数据与调试记录不保留。

## 一、适用口径与设计依据

**覆盖**：IDE / 浏览器 / 资源管理器 / Office 等普通应用窗口最大化、任务栏保持可见；便签始终浮在工作区右上角，不侵入任务栏；热键显隐、纯文本编辑与复制保真、自动保存。

**明确不覆盖**（不做设计、不做测试、不做承诺）：F11 / 独占全屏、PPT 放映、DXGI 全屏游戏、跨虚拟桌面跟随、提权窗口、DRM 覆盖层、多屏混合 DPI 的复杂调度。

**设计依据**：日常最大化的应用窗口是**非 topmost 窗口**，Windows 的层级带保证 topmost 便签永远在它上面，因此只需创建时设置一次 `WS_EX_TOPMOST`，无需定时器或层级重夺。窗口坐标取 `SPI_GETWORKAREA` 工作区矩形而非屏幕矩形，避免侵入任务栏。macOS 版依赖的 `fullScreenAuxiliary` / `canJoinAllSpaces` 是系统给辅助窗口的特权，Win32 没有对等机制，故 Windows 版不追求压在全屏之上。

## 二、构建与产物

| 项 | 说明 |
|---|---|
| 工具链 | MinGW-w64 / GCC；`g++` 与 `windres` 取自 `PATH`，或用环境变量 `GSTICKY_GXX` 指定 `g++.exe` |
| 构建 | `win\build.bat`：`windres` 把 `win\GSticky.ico` 嵌成资源 → `g++ -mwindows -static` 链接 |
| 产物 | `dist\GSticky.exe`，静态链接单文件，仅依赖系统 DLL（`USER32 / GDI32 / SHELL32 / KERNEL32 / UCRT`），无需 VC++ 运行库 |
| 图标 | 项目根 `icon.png` → `win/make_icon.py` 生成 9 尺寸 `win/GSticky.ico` → `win/GSticky.rc` + `windres` 嵌入 exe |
| 产物目录 | `dist/` 为 macOS 与 Windows 共用，已加入 `.gitignore`，不随仓库提交 |

实现对齐 macOS 版规格：工作区右上角置顶便签（per-monitor v2 + DPI 缩放）、纯文本 Edit（补齐 `Ctrl+A`）、托盘三菜单、`Ctrl+Alt+G` 热键、44px 分层圆形悬浮球（3px 判别拖动、按显示器记忆落点、`WS_EX_NOACTIVATE`）、0.5s debounce 原子写、红叉=隐藏、单实例互斥、UTF-8 无 BOM + LF 与 macOS 共用 `note.txt`。

**图标一致性**：exe 文件图标、窗口 / 任务栏、系统托盘、悬浮球中心图案全部取自同一份 `GSticky.ico` 资源，与 macOS 版共用 `icon.png`；小尺寸用 DIB 帧保证兼容，大尺寸用 PNG 帧压缩；DPI 变化时按新缩放重取图标并重绘悬浮球。悬浮球为「系统色圆底 + 无描边 + `DrawIconEx` 居中画品牌云标」，与 macOS `BallView.draw` 同语义。

## 三、验证方式（可复现）

```bat
dist\GSticky.exe --selftest
```
```bash
python win/verify_exe.py
python win/verify_icon.py
```

> 验证脚本使用 `PATH` 中的系统 Python；不依赖第三方包。

| 命令 | 覆盖内容 |
|---|---|
| `GSticky.exe --selftest` | 内建静态自检：topmost / toolwindow / 工作区内 / 真实 z-order 压住后建的最大化窗口 / Edit / 热键 / 托盘 / 悬浮球 / NOACTIVATE / UTF-8+LF 往返 / 图标来自资源 / 悬浮球图标来自资源。另输出 dpi、工作区与便签矩形几何数据；写入 `%APPDATA%\GSticky\selftest.json`，用独立临时文件，不碰 `note.txt`。 |
| `win/verify_exe.py` | 真实启动的端到端回归：置顶与工作区几何 → `WM_CHAR` 真实输入触发 0.5s 自动保存 → UTF-8 无 BOM、仅 LF、精确落盘 → 热键显隐 → 球与便签互斥、球不抢前台焦点 → `PrintWindow(PW_RENDERFULLCONTENT)` 抓悬浮球做像素断言（近白圆底占比达标、品牌青绿像素存在、无旧黄底 / 深色方块）→ 托盘退出 `exit_code=0`。自动备份并原样还原 `note.txt`，并在系统临时目录输出 `gsticky_ball_preview.png` 供肉眼核对。 |
| `win/verify_icon.py` | 校验 exe 内 `RT_GROUP_ICON` / `RT_ICON` 帧数与尺寸，32×32（DIB 帧）和 256×256（PNG 帧）均可被系统加载且含非透明像素，导出 256 预览 PNG 供核对图案与透明背景。 |

悬浮球必须用 `PrintWindow` 抓窗口自身绘制结果：它是 `WS_EX_LAYERED` 窗口，由 DWM 独立合成，直接 `GetDC(NULL)+BitBlt` 抓屏只会得到桌面壁纸。抓取有效性判据为「近白圆底占比达标」，避免把背景误判成品牌色。

## 四、已知限制

- 「复制全部」端到端检查默认跳过：剪贴板含 `DataObject` / `Ole Private Data` 等 OLE 私有格式时，强行备份恢复有损坏用户数据的风险；`python win/verify_exe.py --clipboard` 可强制（会替换剪贴板）。剪贴板被其它进程占用时同样跳过并标注，不算失败。代码路径只写 `CF_UNICODETEXT`，不写 RTF / HTML / 位图。
- 悬浮球圆沿用 GDI `Ellipse` 绘制，无抗锯齿；macOS 版 `NSBezierPath` 有抗锯齿，高分屏下 Windows 圆形边缘略呈阶梯，不影响图案与功能。
- 真实应用窗口最大化、中文输入法、多屏混合 DPI、长时间运行稳定性，仍需日常使用确认。
- macOS 构建脚本需在 macOS 环境执行（依赖 `swiftc` / `sips` / `iconutil` / `codesign`），Windows 侧只做语法与路径可达性核对。
