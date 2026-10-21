# GSticky

> 极简的纯文本浮动便签。**全屏干活时，把临时想法随手记在屏幕角落——不切出全屏、不打断思路。**

GSticky 是一款用 Swift / AppKit **原生**开发的 macOS 便签。它只做一件事，并把它做对：**在你做任何事的窗口之上，安静地浮着一个纯文本输入框。**

## 为什么有 GSticky

市面上的悬浮便签（如 DSticky）很好用，但有个让人抓狂的毛病：编辑时自动给我加格式，然后从它里面复制内容时**格式全丢**——粘到别处只剩光秃秃的字。

根因很简单：这类工具内部是富文本编辑器，复制时会把你的内容**重写一遍样式**，源格式自然就没了。

GSticky 的解法是**治本**——内部只用纯文本存储，从根上不存在「格式被改写」。复制出去的就是你写进去的逐字内容（含换行、缩进、空格），100% 保真。

## 特性

- 🟢 **真·压在全屏之上**：窗口层级设为 `NSWindow.Level(100)` 并开启 `fullScreenAuxiliary` + `canJoinAllSpaces`，即使你正在全屏写代码 / 看视频 / 演示，便签也始终浮在最上层、跟随所有 Space。
- 📝 **纯文本编辑，零格式干扰**：没有加粗 / 颜色 / 字体按钮，粘进来的富文本自动转纯文本。你看到的就是你得到的。
- 📋 **复制原样保真**：⌘C 即纯文本；额外提供「复制全部（纯文本）」菜单项，只向剪贴板写入字符串，绝不夹带 RTF / 样式。
- ⌨️ **全局热键**：`Ctrl + Option + G` 一键显隐（首次需在「系统设置 → 隐私与安全性 → 辅助功能」中授权 GSticky）。
- 📌 **菜单栏常驻**：无 Dock 图标，只在菜单栏留一个 📌，轻量不喧宾夺主。
- 💾 **自动保存**：内容实时落盘 `~/Library/Application Support/GSticky/note.txt`，重启不丢。
- 🪶 **极致轻量**：原生 AppKit 实现，无 Electron、无外部依赖，单二进制仅几百 KB。

## 使用场景

> 写代码写到一半，突然想起要回个邮件 / 记个 TODO / 存个链接——

**传统做法**：退出全屏 → 打开备忘录 → 记下 → 切回全屏。思路被打断三次。

**GSticky 做法**：`Ctrl + Option + G` 唤起浮层 → 敲两行 → 再按一次隐去。全程不离开当前窗口。

这就是它存在的意义：**全屏工作时当一块「临时草稿纸」，用完即隐，绝不抢戏。**

## 安装

> 当前以源码构建方式分发（MIT 开源，欢迎 PR / Issue）。后续可能提供 Homebrew 或预编译包。

### 从源码构建

前置条件：安装 **Xcode Command Line Tools**（`xcode-select --install`），需 **macOS 13.0+**。

```bash
git clone https://github.com/zzjhacn/GSticky.git
cd GSticky
bash build.sh
```

`build.sh` 会依次：① 用 `swiftc` 编译 → ② 手工组装 `.app` 包 → ③ ad-hoc 签名并去除隔离属性 → ④ 启动 GSticky。

构建产物位于 `build/GSticky.app`，可直接拖入「应用程序」文件夹长期使用。

> Apple Silicon 用户开箱即用；Intel Mac 请将 `build.sh` 中的 `-target arm64-apple-macosx13.0` 改为 `-target x86_64-apple-macosx13.0` 再构建。

## 使用说明

| 操作          | 方式                                                        |
| ----------- | --------------------------------------------------------- |
| 显示 / 隐藏便签   | 菜单栏图标 →「显示 / 隐藏便签」，**或**全局热键 `Ctrl + Option + G`          |
| 复制全部内容（纯文本） | 菜单栏图标 →「复制全部（纯文本）」                                        |
| 退出          | 菜单栏图标 →「退出 GSticky」                                       |
| 移动 / 缩放便签   | 直接拖标题栏 / 拉窗口边缘（默认停靠屏幕右上角）                                 |
| 编辑          | 直接在便签里打字                                                  |
| 标准编辑快捷键     | `⌘A` 全选 · `⌘C` 复制 · `⌘V` 粘贴 · `⌘X` 剪切 · `⌘Z` 撤销 · `⌘Q` 退出 |

> 提示：在**目标 App** 里粘贴时若变样，那是目标 App 的行为；可改用「粘贴并匹配样式」（`⌥⇧⌘V`），或先过一遍 GSticky 再粘——纯文本怎么会丢格式呢。

## 数据存哪

所有内容保存在一个纯文本文件里：

```
~/Library/Application Support/GSticky/note.txt
```

UTF-8 明文，一份文件，随便备份 / 同步（接 iCloud、Dropbox 软链都行）。

## 自定义（可选）

源码 `Sources/main.swift` 顶部有两个可调常量：

```swift
// 窗口层级：若实测仍被个别全屏 App 盖住，调大即可（如 1000）
let stickyWindowLevel = NSWindow.Level(rawValue: 100)

// 全局热键按键（当前为 'G'，配合 Ctrl + Option 修饰键）
let hotkeyKeyCode: UInt16 = 5
```

改完重新 `bash build.sh` 生效。

**应用图标**：把 200×200 的 PNG 放到项目根目录并命名 **`icon.png`**（同时兼容旧的 `logo200.png` / `logo.png`），重新运行 `bash build.sh` 即会生成 `AppIcon.icns` 写入应用包；菜单栏图标会自动去白底、裁边后生成 `menubar.png`。

## 技术栈

- 语言：Swift（AppKit / Cocoa）
- 打包：手工拼接 `.app` + `codesign` ad-hoc 签名
- 依赖：**无**（零第三方库、零运行时依赖）

## 开源协议

本项目以 **MIT 协议** 开源，LICENSE 文件由作者提供。

---

© GSticky
