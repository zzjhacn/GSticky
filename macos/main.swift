import Cocoa

// MARK: - 可调常量
// 窗口层级：调高即可压在全屏 App 之上。若实测仍被全屏盖住，把数值调大（如 1000）。
// 配合 collectionBehavior 的 .fullScreenAuxiliary + .canJoinAllSpaces 才能跨 Space 常驻。
let stickyWindowLevel = NSWindow.Level(rawValue: 100)

// 全局热键：Ctrl + Option + G 切换便签显隐（需系统「辅助功能」授权才生效）
let hotkeyKeyCode: UInt16 = 5 // 'G'

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTextViewDelegate {

    private var window: NSWindow!
    private var textView: NSTextView!
    private var statusItem: NSStatusItem!
    private var statusButtonImage: NSImage?
    private var saveURL: URL!
    private var saveTimer: Timer?
    private var globalMonitor: Any?
    private var localMonitor: Any?
    private var ballWindow: NSWindow!

    // MARK: - 生命周期
    func applicationDidFinishLaunching(_ notification: Notification) {
        setupSaveURL()
        buildMainMenu()
        buildStatusItem()
        buildWindow()
        buildBall()
        setupScreenFollowing()
        loadNote()
        setupHotkey()
        // 应用图标（Finder / 强制退出窗口可见；菜单栏应用无 Dock 图标）
        NSApp.applicationIconImage = NSImage(named: "icon")
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        saveNow()
        return .terminateNow
    }

    // MARK: - 存储路径
    private func setupSaveURL() {
        let dir = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("GSticky", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        saveURL = dir.appendingPathComponent("note.txt")
    }

    // MARK: - 主菜单（保证标准编辑快捷键 ⌘A/⌘C/⌘V/⌘X 与 ⌘Q 可用）
    private func buildMainMenu() {
        let mainMenu = NSMenu()

        // App 菜单：⌘Q 退出
        let appMenuItem = NSMenuItem()
        let appMenu = NSMenu(title: "GSticky")
        appMenu.addItem(withTitle: "退出 GSticky",
                        action: #selector(NSApplication.terminate(_:)),
                        keyEquivalent: "q")
        appMenuItem.submenu = appMenu
        mainMenu.addItem(appMenuItem)

        // 编辑菜单：⌘Z / ⇧⌘Z / ⌘X / ⌘C / ⌘V / ⌘A
        let editMenuItem = NSMenuItem()
        let editMenu = NSMenu(title: "编辑")

        // 撤销 / 重做：直接作用于文本框的 undoManager，保证点了有效
        let undoItem = NSMenuItem(title: "撤销", action: #selector(performUndo(_:)), keyEquivalent: "z")
        undoItem.target = self
        let redoItem = NSMenuItem(title: "重做", action: #selector(performRedo(_:)), keyEquivalent: "Z")
        redoItem.target = self
        editMenu.addItem(undoItem)
        editMenu.addItem(redoItem)
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editMenuItem.submenu = editMenu
        mainMenu.addItem(editMenuItem)

        NSApp.mainMenu = mainMenu
    }

    @objc private func performUndo(_ sender: Any?) {
        textView?.undoManager?.undo()
    }

    @objc private func performRedo(_ sender: Any?) {
        textView?.undoManager?.redo()
    }

    // MARK: - 菜单栏
    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            if let logo = NSImage(named: "menubar") ?? NSImage(named: "icon") {
                let icon = logo.copy() as? NSImage
                icon?.size = NSSize(width: 18, height: 18)
                icon?.isTemplate = false
                button.image = icon
                button.imagePosition = .imageOnly
                statusButtonImage = icon
            } else {
                button.title = "📌"
            }
            button.toolTip = "GSticky · 纯文本浮动便签"
        }
        let menu = NSMenu()
        let toggle = NSMenuItem(title: "显示 / 隐藏便签", action: #selector(toggleWindow), keyEquivalent: "")
        let copyAll = NSMenuItem(title: "复制全部（纯文本）", action: #selector(copyAll), keyEquivalent: "")
        let quit = NSMenuItem(title: "退出 GSticky", action: #selector(quit), keyEquivalent: "q")
        menu.items = [toggle, copyAll, .separator(), quit]
        statusItem.menu = menu
    }

    @objc private func toggleWindow() {
        setNoteVisible(!window.isVisible)
    }

    // 统一的显隐状态机：便签与悬浮球互斥（显示其一即隐藏另一）
    private func setNoteVisible(_ visible: Bool) {
        if visible {
            window.makeKeyAndOrderFront(nil)
            ballWindow.orderOut(nil)
        } else {
            window.orderOut(nil)
            positionBallOnActiveScreen()
            // 用 orderFront 而非 makeKey，避免抢走前台 App 焦点
            ballWindow.orderFront(nil)
        }
    }

    // 拦截红叉关闭：便签是常驻草稿，关闭 = 隐藏（与热键/菜单隐藏一致），并唤起悬浮球
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        setNoteVisible(false)
        return false
    }

    @objc private func copyAll() {
        let text = textView.string
        let pb = NSPasteboard.general
        // 只写 string 类型，确保原样复制、不夹带富文本/RTF
        pb.clearContents()
        pb.setString(text, forType: .string)
        flashStatus(title: "✅ 已复制")
    }

    @objc private func quit() {
        NSApplication.shared.terminate(nil)
    }

    // MARK: - 浮动窗口
    private func buildWindow() {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 320, height: 260),
            styleMask: [.titled, .closable, .resizable, .utilityWindow],
            backing: .buffered,
            defer: false
        )
        window.title = "GSticky"
        window.isReleasedWhenClosed = false
        window.delegate = self   // 接管关闭事件：红叉 = 隐藏便签 + 显示悬浮球
        // 关键①：高窗口层级 → 压在最上层
        window.level = stickyWindowLevel
        // 关键②：跨所有 Space（含全屏 Space）常驻
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        window.backgroundColor = NSColor.windowBackgroundColor
        window.hasShadow = true

        let scroll = NSTextView.scrollableTextView()
        // 让滚动视图填满窗口内容区并随窗口缩放
        let contentSize = window.contentRect(forFrameRect: window.frame).size
        scroll.frame = NSRect(x: 0, y: 0, width: contentSize.width, height: contentSize.height)
        scroll.autoresizingMask = [.width, .height]
        if let tv = scroll.documentView as? NSTextView {
            textView = tv
            // 关键③：纯文本编辑，无格式控制
            textView.isRichText = false
            textView.importsGraphics = false
            textView.allowsUndo = true
            textView.font = NSFont.systemFont(ofSize: 13)
            textView.textContainerInset = NSSize(width: 6, height: 6)
            textView.delegate = self
        }
        window.contentView = scroll

        positionTopRight()
        window.makeKeyAndOrderFront(nil)
    }

    private func positionTopRight() {
        guard let screen = NSScreen.main else { return }
        let f = window.frame
        let x = screen.visibleFrame.maxX - f.width - 20
        let y = screen.visibleFrame.maxY - f.height - 20
        window.setFrameOrigin(NSPoint(x: x, y: y))
    }

    // MARK: - 悬浮球（便签隐藏时出现的入口）
    private func buildBall() {
        let size = NSSize(width: 44, height: 44)
        ballWindow = NSWindow(
            contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        // 与便签同层级，保证在全屏 App 之上也可见；canJoinAllSpaces 让它在所有桌面/全屏 Space 都出现，
        // 配合下方「活动屏跟随」只锚定到当前活动屏幕，等价于「当前活动屏幕右上角」
        ballWindow.level = stickyWindowLevel
        ballWindow.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        ballWindow.backgroundColor = .clear
        ballWindow.isOpaque = false
        ballWindow.hasShadow = false
        ballWindow.ignoresMouseEvents = false
        ballWindow.isMovableByWindowBackground = false

        let ballView = BallView()
        ballView.onTap = { [weak self] in self?.setNoteVisible(true) }
        ballView.onDragEnd = { [weak self] in self?.persistBallPosition() }
        ballWindow.contentView = ballView

        // 启动时不显示球（便签默认可见），仅当便签被隐藏后才出现
        ballWindow.orderOut(nil)
    }

    // 当前活动屏幕的唯一键（用于分屏记忆落点）
    private func screenKey() -> String {
        if let id = NSScreen.main?.deviceDescription[NSDeviceDescriptionKey(rawValue: "NSScreenNumber")] as? UInt32 {
            return "\(id)"
        }
        return "default"
    }

    // 把悬浮球放到当前活动屏幕的右上角（有记忆落点则优先）
    private func positionBallOnActiveScreen() {
        guard let screen = NSScreen.main else { return }
        let r: CGFloat = 44
        let margin: CGFloat = 16
        let vf = screen.visibleFrame
        var x = vf.maxX - r - margin
        var y = vf.maxY - r - margin
        if let saved = UserDefaults.standard.string(forKey: "ballPos_\(screenKey())") {
            let parts = saved.split(separator: ",").compactMap { Double($0) }
            if parts.count == 2 { x = CGFloat(parts[0]); y = CGFloat(parts[1]) }
        }
        ballWindow.setFrameOrigin(NSPoint(x: x, y: y))
    }

    // 拖动结束后，按屏幕记忆当前落点
    private func persistBallPosition() {
        let o = ballWindow.frame.origin
        UserDefaults.standard.set("\(o.x),\(o.y)", forKey: "ballPos_\(screenKey())")
    }

    // 活动屏 / 切屏 / 热插拔显示器时，若球正显示则重定位到新活动屏
    private func setupScreenFollowing() {
        let nc = NSWorkspace.shared.notificationCenter
        nc.addObserver(self, selector: #selector(activeScreenChanged),
                       name: NSWorkspace.activeSpaceDidChangeNotification, object: nil)
        nc.addObserver(self, selector: #selector(activeScreenChanged),
                       name: NSWorkspace.didActivateApplicationNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(activeScreenChanged),
                       name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

    @objc private func activeScreenChanged() {
        if ballWindow.isVisible {
            positionBallOnActiveScreen()
        }
    }

    // MARK: - 载入 / 保存
    private func loadNote() {
        guard let data = try? Data(contentsOf: saveURL),
              let str = String(data: data, encoding: .utf8) else { return }
        textView.string = str
    }

    private func saveNow() {
        guard let str = textView?.string else { return }
        try? str.write(to: saveURL, atomically: true, encoding: .utf8)
    }

    func textDidChange(_ notification: Notification) {
        saveTimer?.invalidate()
        saveTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak self] _ in
            self?.saveNow()
        }
    }

    // MARK: - 全局热键
    private func setupHotkey() {
        let isHot = { (ev: NSEvent) -> Bool in
            ev.modifierFlags.contains(.control) && ev.modifierFlags.contains(.option) && ev.keyCode == hotkeyKeyCode
        }
        // 全局监听：仅在「其他 App 处于激活」时触发（如全屏应用中呼出）
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .keyDown) { [weak self] ev in
            if isHot(ev) { self?.toggleWindow() }
        }
        // 本地监听：GSticky 自身激活时也能用；其余按键原样放行（保证 ⌘A/C/V/X/Q 正常）
        localMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] ev in
            if isHot(ev) {
                self?.toggleWindow()
                return nil
            }
            return ev
        }
    }

    // MARK: - 状态栏反馈
    private func flashStatus(title: String) {
        guard let button = statusItem.button else { return }
        button.image = nil
        button.title = title
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            button.title = ""
            button.image = self?.statusButtonImage
        }
    }
}

// MARK: - 悬浮球视图（圆形 + 拖动 / 点击判别）
final class BallView: NSView {
    var onTap: (() -> Void)?
    var onDragEnd: (() -> Void)?
    private var dragStart: NSPoint?
    private var windowStart: NSPoint?
    private let moveThreshold: CGFloat = 3

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let r = bounds
        let path = NSBezierPath(ovalIn: r)
        // 半透明系统色（自动适配深浅色），仅画出圆形，超出圆形的角落天然透明；无描边，避免深色模式下出现黑边
        NSColor.windowBackgroundColor.withAlphaComponent(0.92).setFill()
        path.fill()
        // 中心放品牌云标
        if let img = NSImage(named: "icon") {
            let s: CGFloat = 24
            img.draw(in: NSRect(x: (r.width - s) / 2, y: (r.height - s) / 2, width: s, height: s))
        }
    }

    override func mouseDown(with event: NSEvent) {
        dragStart = event.locationInWindow
        windowStart = window?.frame.origin
    }

    override func mouseDragged(with event: NSEvent) {
        guard let dragStart, let windowStart, let win = window else { return }
        let cur = event.locationInWindow
        win.setFrameOrigin(NSPoint(x: windowStart.x + cur.x - dragStart.x,
                                  y: windowStart.y + cur.y - dragStart.y))
    }

    override func mouseUp(with event: NSEvent) {
        defer { dragStart = nil; windowStart = nil }
        guard let dragStart else { return }
        let cur = event.locationInWindow
        let dist = hypot(cur.x - dragStart.x, cur.y - dragStart.y)
        if dist < moveThreshold {
            onTap?()        // 视为点击 → 显示便签
        } else {
            onDragEnd?()    // 视为拖动 → 记忆落点
        }
    }
}

// MARK: - 入口
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
