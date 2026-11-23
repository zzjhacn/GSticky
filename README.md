# GSticky

> 极简的纯文本浮动便签。**干活时把临时想法随手记在屏幕角落——不切窗口、不打断思路。**

GSticky 是原生开发的双平台便签：macOS 版用 Swift / AppKit，Windows 版用 C++ / Win32。两个版本共用同一份设计规格、同一个应用图标源 `icon.png`，也共用同一份 `note.txt` 文件格式。它只做一件事，并把它做对：**在你正在用的窗口之上，安静地浮着一个纯文本输入框。**

## 平台与目录

| 路径 | 内容 |
| --- | --- |
| `icon.png` | **双平台唯一图标源**（200×200 PNG，白底） |
| `macos/` | macOS 版源码 `main.swift` + `Screenshot.swift`（区域截图）、构建脚本 `build.sh` |
| `macos/tools/` | macOS 菜单栏图标处理工具 `make_menubar_icon.swift` |
| `win/` | Windows 版源码 `main.cpp`、`screenshot.h` / `screenshot.cpp`（区域截图）、`build.bat`、图标资源 `GSticky.ico` / `.rc`、图标生成与验证脚本、验证记录 `VERIFICATION.md` |
| `dist/` | 双平台构建产物 `GSticky.app` / `GSticky.exe`；本地生成，不随仓库提交 |

## 为什么有 GSticky

市面上的悬浮便签（如 DSticky）很好用，但有个让人抓狂的毛病：编辑时自动给我加格式，然后从它里面复制内容时**格式全丢**——粘到别处只剩光秃秃的字。

根因很简单：这类工具内部是富文本编辑器，复制时会把你的内容**重写一遍样式**，源格式自然就没了。

GSticky 的解法是**治本**——内部只用纯文本存储，从根上不存在「格式被改写」。复制出去的就是你写进去的逐字内容（含换行、缩进、空格），100% 保真。macOS 用 `isRichText=false` 的 `NSTextView`，Windows 用天然纯文本的 Win32 `Edit` 控件并只写 `CF_UNICODETEXT`。

## 特性

**两版共通**

- 📝 **纯文本编辑，零格式干扰**：没有加粗 / 颜色 / 字体按钮，粘进来的富文本自动转纯文本。你看到的就是你得到的。
- 📋 **复制原样保真**：额外提供「复制全部（纯文本）」，只向剪贴板写入字符串，绝不夹带 RTF / 样式。
- ⌨️ **全局热键**：一键显隐（macOS `Ctrl + Option + G`；Windows `Ctrl + Alt + G`）。
- 🔘 **悬浮球入口**：便签隐藏时，当前活动屏幕右上角浮现可拖动的圆形悬浮球；点击唤出（便签显示后球自动消失）。按屏幕记忆落点，不抢前台焦点。圆底取系统色，中心是品牌云标，与 exe / 托盘 / macOS 版同源 `icon.png`。
- 📌 **常驻入口**：macOS 菜单栏图标（无 Dock 图标）；Windows 系统托盘图标。
- 💾 **自动保存**：0.5s debounce 原子写入 `note.txt`，重启不丢。
- 📐 **记住窗口大小与位置**：拖标题栏 / 拉边缘调整后自动保存，下次启动原样恢复；换分辨率或拔掉外接屏后会校验位置仍可见，失效则回退工作区右上角。
- 🖼️ **区域截图（可标注）**：菜单栏 / 托盘一键进入 → 鼠标圈选 → 加文字、画框、箭头，支持单步撤销 → 结果写入剪贴板，可直接粘贴。截图瞬间自动隐藏自家便签与悬浮球，结束后原样恢复，不新增热键。
- 🪶 **极致轻量**：原生实现，无 Electron、无第三方库、无运行时依赖。

**平台差异（重要）**

| 能力 | macOS | Windows |
| --- | --- | --- |
| 置顶范围 | `NSWindow.Level(100)` + `fullScreenAuxiliary` + `canJoinAllSpaces`，**全屏 Space 也压得住** | `WS_EX_TOPMOST`，**只覆盖普通窗口最大化、任务栏可见**的日常场景 |
| 虚拟桌面 / 全屏 Space | 全局跟随 | 不在支持范围内 |
| 唤出焦点 | `makeKeyAndOrderFront`（可直接打字） | 同语义；`win/main.cpp` 顶部 `kActivateOnShow=false` 可改为不抢焦点 |
| 全局热键授权 | 首次需在「系统设置 → 隐私与安全性 → 辅助功能」授权 | `RegisterHotKey` 免授权 |
| 应用图标 | `icon.png` → `AppIcon.icns` / `menubar.png` | `icon.png` → `GSticky.ico`（9 尺寸）经 `windres` 嵌入 exe，窗口 / 任务栏 / 托盘 / 资源管理器同源 |
| 开机自启 | 未提供（可用系统「登录项」自行配置） | 托盘「开机自启」勾选开关，写 `HKCU\...\Run`，**默认关闭** |
| 窗口大小 / 位置记忆 | `UserDefaults` 的 `noteFrame_v1` | `%APPDATA%\GSticky\config.ini` 的 `[note] rect`（逻辑像素） |
| 区域截图取图 | 委托系统 `screencapture -i`，用**系统原生圈选 UI** | `BitBlt` 预抓整个虚拟屏 + 自绘冻结覆盖层圈选 |
| 截图输出 | `NSPasteboard` 的 PNG | `CF_DIB`（32bpp top-down，保留字节置不透明） |
| 分发 | ad-hoc 签名 `.app` | 静态链接单 exe；未签名首次运行可能触发 SmartScreen |

> **Windows 版明确不覆盖**：F11 / 独占全屏、PPT 放映、DXGI 全屏游戏、跨虚拟桌面跟随、提权窗口、DRM 覆盖层。这些不做设计、不做测试、不做承诺。根因是 Windows 的 `WS_EX_TOPMOST` 只是 z-order 层级带，不是 macOS 那种系统给辅助窗口的特权；日常最大化的应用窗口都是非 topmost，一次 `WS_EX_TOPMOST` 就足够，不需要定时器或重夺层级。

## 使用场景

> 写代码写到一半，突然想起要回个邮件 / 记个 TODO / 存个链接——

**传统做法**：退出当前窗口 → 打开备忘录 → 记下 → 切回来。思路被打断三次。

**GSticky 做法**：热键唤起浮层 → 敲两行 → 再按一次隐去。全程不离开当前窗口。

> 更省事的是**悬浮球**：便签隐藏后屏幕右上角会冒出一个小圆球，鼠标点一下就回来，纯鼠标也能随时呼出便签。

> **区域截图操作**：入口 → 拖动鼠标圈选（Esc 取消、选区过小可重选）→ 编辑器里点工具条或按 `1 / 2 / 3` 切换文字 / 画框 / 箭头，`Z`（或 `Ctrl+Z` / `⌘Z`）撤销一步，`Enter` 或「完成」写入剪贴板。文字工具会就地弹出输入框，回车提交。全程不注册新热键，不打断原有 `Ctrl+Alt+G` / `Ctrl+Option+G`。

这就是它存在的意义：**干活时当一块「临时草稿纸」，用完即隐，绝不抢戏。**

---

## 构建

> 当前以源码构建方式分发（MIT 开源，欢迎 PR / Issue）。两个平台都把产物输出到 `dist/`；该目录已加入 `.gitignore`，不随仓库提交。

### macOS 版

前置：安装 **Xcode Command Line Tools**（`xcode-select --install`），需 **macOS 13.0+**。

```bash
git clone https://github.com/zzjhacn/GSticky.git
cd GSticky
bash macos/build.sh
```

`macos/build.sh` 依次：① `swiftc` 编译 → ② 手工组装 `.app` 包（含 `AppIcon.icns` / `menubar.png`）→ ③ ad-hoc 签名并去除隔离属性 → ④ 启动 GSticky。产物 `dist/GSticky.app`，可拖入「应用程序」长期使用；构建中间产物在 `dist/.build/`，脚本结束时自动清理。

> Apple Silicon 开箱即用；Intel Mac 请把 `macos/build.sh` 中的 `-target arm64-apple-macosx13.0` 改成 `-target x86_64-apple-macosx13.0` 再构建。

### Windows 版

前置：**MinGW-w64**（需要 `g++.exe` 和 `windres.exe`，把工具链的 `bin` 目录加入 `PATH` 即可）。若未加入 `PATH`，可先设环境变量 `GSTICKY_GXX` 指向 `g++.exe` 的绝对路径再运行；脚本会从同目录自动推导 `windres`。

```bat
win\build.bat
```

若工具链未加入 `PATH`，先指定编译器再执行：

```bat
set "GSTICKY_GXX=[path-to-mingw64]\bin\g++.exe"
win\build.bat
```

流程：先用 `windres` 把 `win\GSticky.ico` 嵌成资源，再与 `win\main.cpp` 一起静态链接。产物 `dist\GSticky.exe`，静态链接单文件、编译 0 警告，只依赖系统 DLL（`USER32 / GDI32 / SHELL32 / ADVAPI32 / KERNEL32 / UCRT`），无需 VC++ 运行库、无需安装包。

> `win\GSticky.ico` 已随源码提交，**日常构建不需要 Python**。只有更换 `icon.png` 才用系统 Python 跑 `win\make_icon.py` 重新生成 9 尺寸图标，再执行 `win\build.bat`。

## 使用说明

| 操作 | macOS | Windows |
| --- | --- | --- |
| 显示 / 隐藏便签 | 菜单栏 →「显示 / 隐藏」，或 `Ctrl + Option + G` | 托盘 →「显示 / 隐藏」，或 `Ctrl + Alt + G`，或点悬浮球 |
| 复制全部（纯文本） | 菜单栏 →「复制全部（纯文本）」 | 托盘 →「复制全部（纯文本）」 |
| 区域截图 | 菜单栏 →「区域截图（可标注）」 | 托盘 →「区域截图（可标注）」 |
| 退出 | 菜单栏 →「退出 GSticky」 | 托盘 →「退出 GSticky」 |
| 开机自启 | — | 托盘 →「开机自启」（勾选即开，默认关闭） |
| 关闭便签 | — | 点红叉 = 隐藏（不退出），与 macOS 一致 |
| 移动 / 缩放 | 拖标题栏 / 拉边缘；大小与位置自动记忆，下次启动恢复 | 同左（默认停靠工作区右上角） |
| 悬浮球 | 隐藏便签后出现；拖动调位置（按屏幕记忆），单击唤出 | 同左 |
| 编辑 | 直接打字 | 直接打字 |
| 标准编辑快捷键 | `⌘A / ⌘C / ⌘V / ⌘X / ⌘Z / ⌘Q` | `Ctrl+A / C / V / X / Z`（`Ctrl+A` 由代码补齐） |

> **开机自启（仅 Windows）**：托盘菜单 →「开机自启」，勾选即开、取消即关，**默认关闭**。开关状态就是注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `GSticky` 值（数据为带引号的 exe 绝对路径），不需要管理员权限，也不产生额外配置文件。移动或改名 exe 后，下次启动会把注册表中的旧路径自动刷新为当前路径。直接删除 exe 前请先关掉开关，以免登录时残留一个指向失效路径的启动项。

> **区域截图不覆盖**：Windows 侧 GPU 合成窗口、受保护视频帧、DRM 内容、独占全屏可能抓成黑块（GDI `GetDC(NULL)+BitBlt` 只取合成后的桌面像素，读不到受保护表面）；macOS 侧若系统把「屏幕录制」权限归属到本 App，首次需在「系统设置 → 隐私与安全性 → 屏幕录制」授权并重启。两版都不做滚动截图、窗口识别、自由画笔、马赛克、颜色板、桌面文件保存；输出只有剪贴板。

> 提示：在**目标 App** 里粘贴时若变样，那是目标 App 的行为；可改用「粘贴并匹配样式」，或先过一遍 GSticky 再粘——纯文本怎么会丢格式呢。

## 数据存哪

| 平台 | 路径 |
| --- | --- |
| macOS | `~/Library/Application Support/GSticky/note.txt` |
| Windows | `%APPDATA%\GSticky\note.txt` |

UTF-8 明文、**无 BOM、LF 换行**，一份文件，随便备份 / 同步。两个平台完全共用格式，可跨设备同步；Windows 版载入时会把 LF 转成 Edit 控件要求的 CRLF，保存时再统一回 LF。

Windows 版的开机自启开关不落文件：状态即上述注册表值（存在 = 开，不存在 = 关）。

**窗口几何**：Windows 存在 `%APPDATA%\GSticky\config.ini` 的 `[note] rect`（`x,y,w,h`，逻辑像素；同文件 `[ball]` 段保存悬浮球落点），macOS 存在 `UserDefaults` 的 `noteFrame_v1`。首次启动或记录失效时用默认 320×260 + 工作区右上角。

## 自定义（可选）

| 想改什么 | 改哪里 |
| --- | --- |
| macOS 窗口层级 / 热键按键 | `macos/main.swift` 顶部 `stickyWindowLevel`、`hotkeyKeyCode` |
| Windows 唤出是否抢焦点 / 尺寸 / 保存 debounce | `win/main.cpp` 顶部 `kActivateOnShow`、`kBaseNoteW/H`、`kSaveDebounceMs` |
| 便签默认尺寸 / 恢复时的合法范围 | `win/main.cpp` 顶部 `kBaseNoteW/H`、`kMinNoteW/H`、`kMaxNoteW/H`；`macos/main.swift` 的 `buildWindow` / `restoreNoteFrame` |
| **应用图标（双平台）** | 替换项目根 `icon.png`（200×200 PNG，白底），macOS 重跑 `bash macos/build.sh`；Windows 跑 `win\make_icon.py` 后重跑 `win\build.bat` |

macOS 版图标兼容旧命名 `logo200.png` / `logo.png`；两个平台都会把白底转成透明（macOS 由 `macos/tools/make_menubar_icon.swift` 处理，Windows 由 `win/make_icon.py` 用同一阈值处理），保证小尺寸下不出现白方块。

## 技术栈

| 项 | macOS | Windows |
| --- | --- | --- |
| 语言 | Swift（AppKit / Cocoa） | C++17（原生 Win32） |
| 构建 | `swiftc` + 手工 `.app` + `codesign` ad-hoc | `windres` + `g++ -mwindows -static` |
| DPI | AppKit 自动 | 声明 per-monitor v2，按 DPI 缩放窗口 / 字体 / 图标 |
| 依赖 | **无** | **无**（仅系统 DLL） |

## 验证

```bat
dist\GSticky.exe --selftest
python win/verify_exe.py
python win/verify_exe.py --autostart --geometry
python win/verify_icon.py
```

`--selftest` 把结构化结果写到 `%APPDATA%\GSticky\selftest.json`（16 项核心布尔检查，含悬浮球图标来自资源、开机自启注册表往返、便签几何往返、截图 DIB 内存往返、覆盖层压暗非全黑；自启用专用测试键、几何用独立测试 ini，测完即删，不碰真实设置，也不碰 `note.txt`）；`verify_exe.py` 做真实启动的端到端回归（输入 → 0.5s 自动保存 → 复制 → 显隐 → `PrintWindow` 抓悬浮球做像素断言 → 退出，会自动备份并原样还原 `note.txt` 与 `config.ini`，并在系统临时目录输出 `gsticky_ball_preview.png` 供肉眼核对）；`--autostart` 额外实测托盘开关写入 / 删除注册表；`--geometry` 额外实测改窗口 → 退出 → 重启 → 大小与位置恢复。两个可选检查都会改写真实设置并在结束时原样还原，故默认跳过；`verify_icon.py` 校验 exe 内 9 帧图标资源与 256 大图标可加载。

> 验证脚本请使用 `PATH` 中的系统 Python（`python` 命令），不要使用编辑器 / 工具自带的私有 Python 运行时。
> 完整验证记录见 **`win/VERIFICATION.md`**（唯一权威验证文档）。

## 开源协议

本项目以 **MIT 协议** 开源，LICENSE 文件由作者提供。

© GSticky
