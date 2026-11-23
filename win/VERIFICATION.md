# GSticky Windows 版验证记录

> **唯一权威验证文档**。只保留适用口径、设计依据、可复现的验证方式与已知限制；一次性探测脚本、过程数据与调试记录不保留。

## 一、适用口径与设计依据

**覆盖**：IDE / 浏览器 / 资源管理器 / Office 等普通应用窗口最大化、任务栏保持可见；便签始终浮在工作区右上角，不侵入任务栏；热键显隐、纯文本编辑与复制保真、自动保存。

**明确不覆盖**（不做设计、不做测试、不做承诺）：F11 / 独占全屏、PPT 放映、DXGI 全屏游戏、跨虚拟桌面跟随、提权窗口、DRM 覆盖层、多屏混合 DPI 的复杂调度；区域截图下 GPU 合成窗口 / 受保护视频帧 / DRM 内容可能抓成黑块。

**设计依据**：日常最大化的应用窗口是**非 topmost 窗口**，Windows 的层级带保证 topmost 便签永远在它上面，因此只需创建时设置一次 `WS_EX_TOPMOST`，无需定时器或层级重夺。窗口坐标取 `SPI_GETWORKAREA` 工作区矩形而非屏幕矩形，避免侵入任务栏。macOS 版依赖的 `fullScreenAuxiliary` / `canJoinAllSpaces` 是系统给辅助窗口的特权，Win32 没有对等机制，故 Windows 版不追求压在全屏之上。

## 二、构建与产物

| 项 | 说明 |
|---|---|
| 工具链 | MinGW-w64 / GCC；`g++` 与 `windres` 取自 `PATH`，或用环境变量 `GSTICKY_GXX` 指定 `g++.exe` |
| 构建 | `win\build.bat`：`windres` 把 `win\GSticky.ico` 嵌成资源 → `g++ -mwindows -static` 链接 `win\main.cpp` + `win\screenshot.cpp` |
| 产物 | `dist\GSticky.exe`，静态链接单文件，仅依赖系统 DLL（`USER32 / GDI32 / SHELL32 / ADVAPI32 / KERNEL32 / UCRT`），无需 VC++ 运行库 |
| 图标 | 项目根 `icon.png` → `win/make_icon.py` 生成 9 尺寸 `win/GSticky.ico` → `win/GSticky.rc` + `windres` 嵌入 exe |
| 产物目录 | `dist/` 为 macOS 与 Windows 共用，已加入 `.gitignore`，不随仓库提交 |

实现对齐 macOS 版规格：工作区右上角置顶便签（per-monitor v2 + DPI 缩放）、纯文本 Edit（补齐 `Ctrl+A`）、托盘菜单、`Ctrl+Alt+G` 热键、44px 分层圆形悬浮球（3px 判别拖动、按显示器记忆落点、`WS_EX_NOACTIVATE`）、0.5s debounce 原子写、红叉=隐藏、单实例互斥、UTF-8 无 BOM + LF 与 macOS 共用 `note.txt`。

**图标一致性**：exe 文件图标、窗口 / 任务栏、系统托盘、悬浮球中心图案全部取自同一份 `GSticky.ico` 资源，与 macOS 版共用 `icon.png`；小尺寸用 DIB 帧保证兼容，大尺寸用 PNG 帧压缩；DPI 变化时按新缩放重取图标并重绘悬浮球。悬浮球为「系统色圆底 + 无描边 + `DrawIconEx` 居中画品牌云标」，与 macOS `BallView.draw` 同语义。

**开机自启（Windows 专属）**：托盘菜单新增带勾选的「开机自启」项，**默认关闭**；勾选即写、取消即删 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `GSticky` 值（`REG_SZ`，数据为带引号的 exe 绝对路径）。注册表值是开关的唯一状态源，菜单每次现读现显示，不另存配置文件，避免两份状态不一致。选 `HKCU` 而非 `HKLM` / 任务计划 / 服务，是为了无需管理员权限、不引入额外组件、保持单 exe 分发。exe 被移动或改名后，主实例启动时会把旧路径幂等刷新为当前路径；路径比较大小写不敏感（仅盘符大小写不同不会重写注册表），刷新只替换引号内的路径、保留用户追加的启动参数；开关未开启时完全不碰注册表，非本程序写出的命令格式也不擅自接管。自检在专用测试键上完成往返并测完即删，绝不读写用户真实的 Run 项。

**区域截图（双平台，只写剪贴板）**：入口为菜单栏 / 托盘 / 悬浮球右键菜单的「区域截图（可标注）」与截图热键（见下段）。悬浮球右键菜单只有这一项：Windows 侧球是 `WS_EX_NOACTIVATE` 当不了菜单属主，借隐藏的便签做属主（与托盘菜单同一套路）；macOS 侧给 `BallView` 挂 `NSMenu`，由 NSView 默认 `rightMouseDown` 弹出。会话是主消息循环里的非阻塞状态机，不嵌套 `GetMessage` 循环；托盘 / 悬浮球菜单命令先 `PostMessage` 延迟到 `TrackPopupMenu` 返回后再启动，避免把菜单本身截进图里。调用方在启动前隐藏自家便签与悬浮球，收到 `kMsgShotDone` 后按进入前的可见性精确还原，并把前台还给截屏前正在用的窗口（不用 `SetNoteVisible`，避免触发保存与悬浮球重定位）。Windows 侧在隐藏浮层后等 DWM 合成一帧，再用 `GetDC(NULL)` + `CreateCompatibleBitmap(屏幕 DC)` + `BitBlt(SRCCOPY|CAPTUREBLT)` 把整个虚拟屏抓进内存位图，之后才显示覆盖层；全程物理像素，不复用便签的 `S()/L()`（那是系统 DPI 逻辑像素）。覆盖层显示冻结画面，选区外用 `GdiAlphaBlend`（从已链接的 `gdi32.dll` 取；`msimg32.dll` 导出的是 `AlphaBlend`，两者导出名不同）以约 43% 黑压暗、选区画亮框；压暗不可用时**保留底图、绝不涂黑**，两个窗口类都不设背景刷，且显示前先填好内存位图，避免首帧刷黑或未初始化位图造成黑闪。编辑器窗口把裁剪图按显示缩放绘制，标注坐标存**原图像素**，导出时按 1:1 还原，避免缩放后像素错位。输出为 32bpp top-down `CF_DIB`（`BITMAPINFOHEADER` + 像素），保留的第 4 字节统一填 `0xFF`（`GetDIBits` 从 DDB 取回时通常为 0，WPF / 部分浏览器会把它当 alpha 解释成整图全透明），`OpenClipboard` 失败最多重试 3 次；会话结束必须先销毁内存 DC 再删位图，反过来 `DeleteObject` 会失败、每次会话泄漏 4 个 GDI 位图。启动阶段就把 `active` 置位，避免 `Sleep` / 4K 抓屏期间重复进入。只做文字 / 画框 / 箭头与单步撤销，不做桌面文件、自由画笔、马赛克、颜色板。两版在会话期间都挂起便签显隐入口（Windows 的 `ToggleNote()` / `kMsgActivate`，macOS 的 `toggleWindow()`），避免热键或托盘点击把浮层重新弹出来挡住覆盖层。macOS 侧同样在隐藏浮层后等一个合成帧再拉起 `screencapture`，入口做防重入（二次点击若仍先隐藏浮层，会收不到 `restore` 回调导致浮层永久消失）。

**截图热键（双平台）**：macOS `⇧⌘A`、Windows `Ctrl+Shift+A`，直达区域截图会话。两版都用**独占注册**（Windows `RegisterHotKey` + `MOD_NOREPEAT`；macOS Carbon `RegisterEventHotKey`，与显隐热键的 NSEvent 被动监听是两套机制）：只有独占注册才能检测冲突，且按键不再落到前台 App——代价是 GSticky 运行期间应用内同名快捷键（Chrome/Edge「搜索标签页」、Finder「应用程序」等）被全局接管，这是已确认接受的取舍。macOS 侧 Carbon 热键**无需辅助功能权限**。冲突处理口径：启动时注册失败不视为错误；每次开始截图先重试注册（占用方退出后即自愈），仍冲突则把「热键被其他程序占用」追加到会话结束的提示里（Windows 托盘 tip / macOS 菜单栏 flash）。**两系统都没有公开 API 能查出占用热键的进程名**（Windows 只有错误码 1409，macOS 只有 `eventHotKeyExistsErr`），故提示只到「被占用」这一层，不做启发式猜测。会话进行中热键触发被 `IsActive` 防重入挡住；退出时反注册。自检 `allOk` 不纳入截图热键注册结果——被外部程序占用是合法环境状态，不是产品缺陷。

**便签窗口几何记忆（双平台）**：拖动 / 缩放结束与退出时保存 `x,y,w,h`，下次启动恢复；首次启动或记录失效时回退默认 320×260 + 工作区右上角。Windows 写 `%APPDATA%\GSticky\config.ini` 的 `[note] rect`（**逻辑像素**，与 `[ball]` 悬浮球落点同文件），靠 `WM_EXITSIZEMOVE` 与 `WM_DESTROY` 落盘，不在 `WM_SIZE` / `WM_MOVE` 里逐条写；macOS 写 `UserDefaults` 的 `noteFrame_v1`，靠 `windowDidMove` / `windowDidEndLiveResize` 与退出兜底。macOS 不用 `frameAutosaveName`，因为红叉是隐藏而非关闭，窗口不走 AppKit 的自动保存路径。两平台恢复前都做可见性校验（中心点落在某块屏幕、与该屏工作区 / 可见区域至少 1/4 面积交集），拔屏或改分辨率后不会把窗口丢到屏幕外。Windows 按逻辑像素存取，换 DPI 后视觉尺寸保持一致；自检用独立测试 ini、端到端用备份还原，均不污染用户真实配置。

## 三、验证方式（可复现）

```bat
dist\GSticky.exe --selftest
```
```bash
python win/verify_exe.py
python win/verify_exe.py --autostart --geometry
python win/verify_icon.py
```

> 验证脚本使用 `PATH` 中的系统 Python；不依赖第三方包。`--autostart` / `--geometry` 为可选检查，会短暂改写用户真实的自启项 / 配置文件，结束时一律原样还原。

| 命令 | 覆盖内容 |
|---|---|
| `GSticky.exe --selftest` | 内建静态自检 16 项：topmost / toolwindow / 工作区内 / 真实 z-order 压住后建的最大化窗口 / Edit / 热键 / 托盘 / 悬浮球 / NOACTIVATE / UTF-8+LF 往返 / 图标来自资源 / 悬浮球图标来自资源 / 开机自启注册表往返 / 便签几何往返 / **截图 DIB 内存往返**（`shot_dib_roundtrip`，只验证位图 → DIB 字节 → `CreateDIBitmap` 回读的 RGB 一致性）/ **覆盖层压暗非全黑**（`shot_dim_not_black`，纯内存白底经 43% 黑叠加后应为约 145；取不到 `GdiAlphaBlend` 直接判失败）。两项都不建窗口、不抓屏、不碰剪贴板。自启检查在专用测试键、几何检查在独立测试 ini 上完成「写 → 读 → 删 → 确认消失」并测完即删，绝不读写用户真实的 Run 项或 `config.ini`；另只读输出当前开关状态供诊断。结果写入 `%APPDATA%\GSticky\selftest.json`，并附 dpi、工作区与便签矩形几何数据；用独立临时文件，不碰 `note.txt`。 |
| `win/verify_exe.py` | 真实启动的端到端回归：置顶与工作区几何 → `WM_CHAR` 真实输入触发 0.5s 自动保存 → UTF-8 无 BOM、仅 LF、精确落盘 → 热键显隐 → 球与便签互斥、球不抢前台焦点 → `PrintWindow(PW_RENDERFULLCONTENT)` 抓悬浮球做像素断言（近白圆底占比达标、品牌青绿像素存在、无旧黄底 / 深色方块）→ 托盘退出 `exit_code=0`。自动备份并原样还原 `note.txt` 与 `config.ini`，并在系统临时目录输出 `gsticky_ball_preview.png` 供肉眼核对。**启动任何窗口之前**先只读 exe 字节，断言截图子系统的两个窗口类名与菜单项已链接进产物（`shot_build_present`）；区域截图的圈选 / 标注需人工交互，自动化会抢占全屏、隐藏浮层并改写剪贴板，故不做 GUI 自动回归。 |
| `win/verify_exe.py --autostart` | 在上一条基础上，真实发送两次托盘「开机自启」命令，验证 Run 值写入 / 删除、状态翻转、命令带引号且指向当前 exe；**无论成败都在 `finally` 中把用户原本的 Run 项原样还原**（含值与类型）。因会短暂改写真实自启设置，默认不跑。 |
| `win/verify_exe.py --geometry` | 在基础回归之外，真实用 `SetWindowPos` 改动便签大小与位置 → 发 `WM_EXITSIZEMOVE` 触发落盘 → 托盘退出 → 重启 exe → 断言矩形恢复（容差覆盖 DPI 逻辑像素换算与 DWM 不可见边框）；同时确认 `[note] rect` 已写入、两次 `clean_exit=0`。**无论成败都在 `finally` 中把 `config.ini` 与 `note.txt` 原样还原**（原本不存在则删除）。因会重启 exe 并改写真实配置，默认不跑。 |
| `win/verify_icon.py` | 校验 exe 内 `RT_GROUP_ICON` / `RT_ICON` 帧数与尺寸，32×32（DIB 帧）和 256×256（PNG 帧）均可被系统加载且含非透明像素，导出 256 预览 PNG 供核对图案与透明背景。 |

验证悬浮球**图案**时必须用 `PrintWindow(PW_RENDERFULLCONTENT)` 抓窗口自身绘制结果：`GetDC(NULL)+BitBlt` 抓的是合成后的整个桌面，窗口像素会与桌面背景、相邻窗口混在一起，无法做「圆内近白占比」这类断言（早期验证脚本因此出现大量假阳性）。2026-11-23 的 P0 实测已确认：在窗口确实可见、取样矩形正确的前提下，`WS_EX_LAYERED + LWA_ALPHA` 窗口**会**出现在 BitBlt 结果中，`SRCCOPY` 与 `CAPTUREBLT` 均如此，两者连续抓取差异为 0；真实悬浮球（白圆底 + 青绿云标）的裁剪放大图即为证据。因此「抓不到分层窗口」不是 BitBlt 的能力限制，早期记录的「只得到桌面壁纸」结论不再作为设计依据。区域截图功能由此确立硬约束：**先抓屏、后显示覆盖层**，且捕获瞬间必须隐藏自家便签 / 悬浮球。

## 四、已知限制

- 「复制全部」端到端检查默认跳过：剪贴板含 `DataObject` / `Ole Private Data` 等 OLE 私有格式时，强行备份恢复有损坏用户数据的风险；`python win/verify_exe.py --clipboard` 可强制（会替换剪贴板）。剪贴板被其它进程占用时同样跳过并标注，不算失败。代码路径只写 `CF_UNICODETEXT`，不写 RTF / HTML / 位图。
- 悬浮球圆沿用 GDI `Ellipse` 绘制，无抗锯齿；macOS 版 `NSBezierPath` 有抗锯齿，高分屏下 Windows 圆形边缘略呈阶梯，不影响图案与功能。
- 开机自启是「当前用户登录后由 Explorer 执行 `HKCU\...\Run` 项」，不是系统服务，也不会在未登录 / 锁屏状态下启动；不支持为所有用户启用、延迟启动或提权运行——这些都需要管理员权限或额外组件，与单 exe 分发的取舍相悖。
- 截图热键冲突只能提示「被其他程序占用」，查不到占用进程名（两系统均无公开查询接口）；热键注册成功后全局独占 `Ctrl+Shift+A` / `⇧⌘A`，应用内同名快捷键在 GSticky 运行期间失效。
- 删除或移动 exe 前应先在托盘关掉「开机自启」：否则登录时会残留一个指向失效路径的启动项，需手动删除 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `GSticky` 值。程序本身不提供卸载器。
- 窗口几何按逻辑像素存取，换 DPI 后视觉尺寸保持一致；多屏混合 DPI 的复杂调度不在覆盖范围内。恢复时只保证窗口「可见可用」（中心点在屏内、至少 1/4 面积落在工作区），不会强行把用户拖到屏幕边缘外的位置夹回工作区；记录损坏或尺寸越界时回退默认 320×260 + 右上角。
- macOS 侧的几何记忆改动**未在 macOS 真机编译 / 运行验证**：本机无 Xcode / `swiftc`，只做静态语法核对与构建脚本路径检查。需在 Mac 上执行 `bash macos/build.sh` 后，实测「拖动 / 缩放 → 退出 → 重启 → 位置与大小恢复」。
- 真实应用窗口最大化、中文输入法、多屏混合 DPI、长时间运行稳定性，仍需日常使用确认。
- macOS 构建脚本需在 macOS 环境执行（依赖 `swiftc` / `sips` / `iconutil` / `codesign`），Windows 侧只做语法与路径可达性核对。

## 五、区域截图（2026-11-23）

**自动验证已通过**

| 项 | 结果 |
|---|---|
| 构建 | `g++ -std=c++17 -O2 -s -mwindows -static -Wall -Wextra` 编译 `main.cpp` + `screenshot.cpp` + 图标资源，**0 警告**，产物 360,960 B |
| `--selftest` | 新增 `shot_dib_roundtrip=true`、`shot_dim_not_black=true`；其余 15 项核心布尔全 true。唯一 false 为 `hotkey_registered`，原因是验证时主实例仍在运行并占用 `Ctrl+Alt+G`，非产品缺陷 |
| 会话冒烟（不写剪贴板、不启动主程序） | 覆盖层创建 → 模拟圈选 → 编辑器创建 → 画框 / 撤销 → Esc 取消 → `active=0`、覆盖层与编辑器窗口均已销毁、DIB 自检 true |
| P0 平台探针 | 虚拟屏 `[0,0,3840,2160]` / DPI 144 / 工作区 `[72,0,3840,2160]`；BitBlt 抓到 2160 行、非黑像素 87.4%，预览为真实桌面；32bpp top-down DIB（40 B 头 + 33,177,600 B 像素）经 `CreateDIBitmap` 回读 4000 像素 RGB 零差异；剪贴板只读探测 12 种格式、序号未变 |
| 复核后修复 | Windows：GDI 位图释放顺序（先 DC 后位图，否则每次会话泄漏 4 个对象）、`CF_DIB` 保留字节填 `0xFF` 防被当全透明、撤销支持单按 `Z`、启动竞态防重入；macOS：入口防重入、隐藏浮层后等一个合成帧、TCC 归属注释改为「待真机 P0 确认」。改后重建 0 警告、`--selftest` 与会话冒烟仍全通过 |
| **实测反馈：圈选前全黑屏（已修复）** | 根因：`InitAlphaBlend()` 从 `msimg32.dll` 取 `GdiAlphaBlend`，实测该 DLL 只导出 `AlphaBlend`，`GdiAlphaBlend` 在 `gdi32.dll`；取不到函数后 `DimRect()` 的退化分支 `FillRect(BLACK_BRUSH)` 把整屏涂成纯黑。连带修掉首帧黑闪（`CreateCompatibleBitmap` 内容未定义 + 窗口类背景刷设为 `BLACK_BRUSH`）：两个窗口类都不设背景刷、显示前先填好内存位图。回归：`--selftest` 新增 `shot_dim_not_black`（纯内存白底压暗到约 145，取不到函数直接判失败）、会话冒烟加「覆盖层显示前后同区域平均亮度」对比 |
| 构建完整性 | 新 exe 实测含 `GStickyShotOverlay_v1` / `GStickyShotEditor_v1` /「区域截图（可标注）」三处 UTF-16 字符串；`win/verify_exe.py` 新增 `shot_build_present` 静态检查 |

**人工验证清单（需在替换 `dist\GSticky.exe` 后执行）**

1. 托盘 →「区域截图（可标注）」→ **先确认整屏不是全黑、能看清桌面内容**（这是 2026-11-23 全黑屏 bug 的首要验收点），拖动圈选 → 确认选区外变暗约 43%、选区边框清晰，且截图中**没有**自家便签 / 悬浮球 / 托盘菜单。
2. 编辑器里分别用 `1 文字`（含中文输入法）、`2 画框`、`3 箭头` 添加标注，`Z` 撤销一次，确认绘制位置与鼠标一致。
3. 点「完成」或按 Enter → 粘贴到微信 / 企业微信 / 浏览器 / Office，确认图像完整、标注可见、Retina 下不糊。
4. 按 Esc 取消 → 确认便签 / 悬浮球的显隐状态与截图前一致，前台窗口回到原来正在用的应用，剪贴板未被改动。
5. 跨屏圈选（有外接屏时）、4K 高分屏圈选大区域、圈选极小区域（应留在覆盖层允许重选）。

**已知限制**：GDI 抓屏取的是合成后的桌面像素，GPU 合成窗口、受保护视频帧、DRM 内容、独占全屏可能呈黑块，已列入「明确不覆盖」；`CF_DIB` 是位图格式，个别只接受 PNG 剪贴板格式的现代应用可能不显示，后续可按需补 `CF_PNG`（需 PNG 编码器）。编辑器窗口为无边框自绘工具条，GDI 画线无抗锯齿。macOS 侧本次只做静态审阅（本机无 `swiftc`），需在 Mac 上 `bash macos/build.sh` 后按同一清单自测，并先执行 P0 权限归属验证。

**本轮未执行的验证（如实说明）**：完整 `win/verify_exe.py` 端到端回归**没有跑**——主实例 `GSticky.exe` 仍在运行，脚本用 `FindWindowW(GStickyNoteWindow_v1)` 会命中用户正在使用的便签窗口，随后向其发送 `WM_SETTEXT` 与托盘退出命令，可能改写便签内容或结束主实例。同理本轮构建产物只写到系统临时目录，未覆盖 `dist\GSticky.exe`（运行中的 exe 不可覆盖）。需先退出主实例、把新 exe 放进 `dist/` 后再跑完整回归。
