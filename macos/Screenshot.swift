import Cocoa
import Darwin

// GSticky macOS 区域截图 —— 系统委托圈选 + 文字/画框/箭头 + PNG 写剪贴板。
//
// 口径（见 README）：
//   - 圈选交给系统 /usr/sbin/screencapture -i，不用 CGWindowListCreateImage
//     （已弃用且被纳入「屏幕录制」TCC）。权限归属由系统工具还是本 App 承担，
//     本机无法验证，需按 P0 脚本在 Mac 真机确认；若归属本 App，首次须在
//     「系统设置 → 隐私与安全性 → 屏幕录制」授权并重启（见 README 不覆盖口径）。
//   - 不用 -c：取消时不会清空用户剪贴板；选中结果写临时 PNG 再由本 App 读回。
//   - 全程保住 Retina 物理像素：绘制与导出都走 NSBitmapImageRep + 指定像素尺寸
//     的 NSGraphicsContext，绝不用 NSImage.lockFocus()（会降成 1x 糊图）。
//   - 只做剪贴板输出，不做桌面文件；不注册新热键。

private enum Tool: Int {
    case text = 0, rect = 1, arrow = 2
}

private struct Annotation {
    var kind: Tool
    var a: NSPoint          // 原图像素坐标（左下原点）
    var b: NSPoint
    var text: String = ""
}

private let kToolbarH: CGFloat = 34
private let kToolLabels = ["1 文字", "2 画框", "3 箭头", "Z 撤销", "↵ 完成", "Esc 取消"]
private let kAccent = NSColor(red: 0.92, green: 0.19, blue: 0.19, alpha: 1.0)

// MARK: - 文字输入框（Enter 提交 / Esc 取消）
private final class InlineTextField: NSTextField {
    var onCancel: (() -> Void)?
    override func cancelOperation(_ sender: Any?) {
        onCancel?()
    }
}

// MARK: - 标注画布
private final class AnnotationView: NSView {
    var imageRep: NSBitmapImageRep
    var annotations: [Annotation] = []
    var tool: Tool = .rect
    var viewScale: CGFloat = 1.0
    var canvasH: CGFloat = 0
    var preview: Annotation?
    var onFinish: ((Bool) -> Void)?

    private var dragStart: NSPoint?
    private var textField: InlineTextField?
    private var textAt: NSPoint = .zero

    init(frame: NSRect, rep: NSBitmapImageRep, scale: CGFloat, canvasHeight: CGFloat) {
        self.imageRep = rep
        self.viewScale = scale
        self.canvasH = canvasHeight
        super.init(frame: frame)
        wantsLayer = false
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) unsupported") }

    override var acceptsFirstResponder: Bool { true }

    private var pixelSize: NSSize {
        NSSize(width: imageRep.pixelsWide, height: imageRep.pixelsHigh)
    }

    private func drawImage(in rect: NSRect) {
        let img = NSImage(size: pixelSize)
        img.addRepresentation(imageRep)
        img.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1.0)
    }

    private func drawOne(_ a: Annotation, scale: CGFloat) {
        let p1 = NSPoint(x: a.a.x * scale, y: a.a.y * scale)
        let p2 = NSPoint(x: a.b.x * scale, y: a.b.y * scale)
        kAccent.setStroke()
        kAccent.setFill()
        let width: CGFloat = max(1.0, 2.0 * scale)
        switch a.kind {
        case .rect:
            let r = NSRect(x: min(p1.x, p2.x), y: min(p1.y, p2.y),
                           width: abs(p2.x - p1.x), height: abs(p2.y - p1.y))
            let path = NSBezierPath(rect: r)
            path.lineWidth = width
            path.stroke()
        case .arrow:
            let path = NSBezierPath()
            path.lineWidth = width
            path.move(to: p1)
            path.line(to: p2)
            path.stroke()
            let angle = atan2(p2.y - p1.y, p2.x - p1.x)
            let len: CGFloat = 10 + width * 3
            let head = NSBezierPath()
            head.move(to: p2)
            head.line(to: NSPoint(x: p2.x - len * cos(angle - 0.45),
                                  y: p2.y - len * sin(angle - 0.45)))
            head.line(to: NSPoint(x: p2.x - len * cos(angle + 0.45),
                                  y: p2.y - len * sin(angle + 0.45)))
            head.close()
            head.fill()
        case .text:
            let font = NSFont.systemFont(ofSize: max(9, 16 * scale))
            (a.text as NSString).draw(at: p1,
                                      withAttributes: [.font: font, .foregroundColor: kAccent])
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let w = bounds.width
        NSColor.windowBackgroundColor.setFill()
        NSBezierPath(rect: NSRect(x: 0, y: canvasH, width: w, height: kToolbarH)).fill()
        let font = NSFont.systemFont(ofSize: 12)
        let bw = w / CGFloat(kToolLabels.count)
        for (i, label) in kToolLabels.enumerated() {
            let r = NSRect(x: CGFloat(i) * bw, y: canvasH, width: bw, height: kToolbarH)
            if i <= 2 && i == tool.rawValue {
                NSColor(calibratedRed: 0.86, green: 0.94, blue: 0.96, alpha: 1).setFill()
                NSBezierPath(rect: r).fill()
            }
            NSColor.separatorColor.setStroke()
            let line = NSBezierPath()
            line.move(to: NSPoint(x: r.maxX, y: r.minY))
            line.line(to: NSPoint(x: r.maxX, y: r.maxY))
            line.stroke()
            var t = r
            t.origin.x += 4
            t.size.width -= 8
            t.origin.y += (kToolbarH - 16) / 2
            t.size.height = 16
            (label as NSString).draw(in: t, withAttributes: [
                .font: font,
                .foregroundColor: (i <= 2 && i == tool.rawValue)
                    ? NSColor(calibratedRed: 0, green: 0.35, blue: 0.43, alpha: 1)
                    : NSColor.labelColor
            ])
        }
        drawImage(in: NSRect(x: 0, y: 0, width: w, height: canvasH))
        for a in annotations { drawOne(a, scale: viewScale) }
        if let p = preview { drawOne(p, scale: viewScale) }
    }

    // MARK: 输入
    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if p.y >= canvasH {
            commitText()
            let idx = min(kToolLabels.count - 1, Int(p.x / (bounds.width / CGFloat(kToolLabels.count))))
            switch idx {
            case 0: tool = .text
            case 1: tool = .rect
            case 2: tool = .arrow
            case 3:
                if !annotations.isEmpty { annotations.removeLast() }
            case 4: onFinish?(true)
            default: onFinish?(false)
            }
            needsDisplay = true
            return
        }
        if tool == .text {
            beginText(at: p)
            return
        }
        commitText()
        dragStart = NSPoint(x: p.x / viewScale, y: p.y / viewScale)
        preview = Annotation(kind: tool, a: dragStart!, b: dragStart!)
        needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard dragStart != nil else { return }
        let p = convert(event.locationInWindow, from: nil)
        preview?.b = NSPoint(x: p.x / viewScale, y: p.y / viewScale)
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        guard dragStart != nil else { return }
        let p = convert(event.locationInWindow, from: nil)
        preview?.b = NSPoint(x: p.x / viewScale, y: p.y / viewScale)
        if let a = preview, abs(a.b.x - a.a.x) >= 4 || abs(a.b.y - a.a.y) >= 4 {
            annotations.append(a)
        }
        preview = nil
        dragStart = nil
        needsDisplay = true
    }

    private func beginText(at p: NSPoint) {
        commitText()
        textAt = NSPoint(x: p.x / viewScale, y: p.y / viewScale)
        let f = InlineTextField(frame: NSRect(x: p.x, y: p.y - 12, width: 220, height: 26))
        f.isEditable = true
        f.isBordered = true
        f.bezelStyle = .roundedBezel
        f.font = NSFont.systemFont(ofSize: 13)
        f.target = self
        f.action = #selector(commitTextAction(_:))
        f.onCancel = { [weak self] in self?.cancelText() }
        addSubview(f)
        textField = f
        window?.makeFirstResponder(f)
    }

    @objc private func commitTextAction(_ sender: Any?) { commitText() }

    private func cancelText() {
        textField?.removeFromSuperview()
        textField = nil
        window?.makeFirstResponder(self)
    }

    private func commitText() {
        guard let f = textField else { return }
        let s = f.stringValue
        f.removeFromSuperview()
        textField = nil
        window?.makeFirstResponder(self)
        if !s.isEmpty {
            annotations.append(Annotation(kind: .text, a: textAt, b: textAt, text: s))
        }
        needsDisplay = true
    }

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 53: onFinish?(false)                       // Esc
        case 36: commitText(); onFinish?(true)          // Return
        case 18: tool = .text
        case 19: tool = .rect
        case 20: tool = .arrow
        default:
            // 单按 Z 或 Cmd+Z 都撤销一步（与 Windows 侧、README 口径一致）
            if event.charactersIgnoringModifiers?.lowercased() == "z",
               !annotations.isEmpty {
                annotations.removeLast()
            }
        }
        needsDisplay = true
    }

    // MARK: 输出（保住物理像素）
    func renderPNG() -> Data? {
        let pw = imageRep.pixelsWide, ph = imageRep.pixelsHigh
        guard let out = NSBitmapImageRep(bitmapDataPlanes: nil,
                                         pixelsWide: pw, pixelsHigh: ph,
                                         bitsPerSample: 8, samplesPerPixel: 4,
                                         hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB,
                                         bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
        out.size = NSSize(width: pw, height: ph)     // 点 = 像素，1:1 不降采样
        guard let ctx = NSGraphicsContext(bitmapImageRep: out) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = ctx
        drawImage(in: NSRect(x: 0, y: 0, width: pw, height: ph))
        for a in annotations { drawOne(a, scale: 1.0) }
        NSGraphicsContext.restoreGraphicsState()
        return out.representation(using: .png, properties: [:])
    }
}

// MARK: - 可成为 key window 的标注窗口
private final class KeyableWindow: NSWindow {
    override var canBecomeKey: Bool { true }
}

// MARK: - 会话控制器
final class ScreenshotController: NSObject {
    static let shared = ScreenshotController()
    private(set) var isActive = false   // 会话期间挂起便签热键，避免浮层重新冒出来

    private var process: Process?
    private var tempURL: URL?
    private var editor: NSWindow?
    private var noteWasVisible = false
    private var ballWasVisible = false
    private var frontApp: NSRunningApplication?
    private var restore: ((Bool, Bool, Bool) -> Void)?

    /// 由 AppDelegate 在隐藏自家便签/悬浮球之后调用。
    /// restore(noteVisible, ballVisible, copied) 在会话结束时必定回调一次。
    func start(noteWasVisible note: Bool, ballWasVisible ball: Bool,
               restore: @escaping (Bool, Bool, Bool) -> Void) {
        guard process == nil, !isActive else { return }   // 防重入
        isActive = true
        noteWasVisible = note
        ballWasVisible = ball
        self.restore = restore
        frontApp = NSWorkspace.shared.frontmostApplication

        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("gsticky_shot", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("selection.png")
        try? FileManager.default.removeItem(at: url)
        tempURL = url

        // 隐藏自家浮层后等一个合成帧再拉起 screencapture：刚 orderOut 的便签 /
        // 悬浮球可能还残留在屏幕像素里被圈进去（与 Windows 侧等 DWM 一帧同理）。
        // isActive 已在函数开头置位，这 0.12s 内再点菜单会被挡住；
        // 若期间 finish() 把 isActive 复位，这段任务就直接放弃，不会拉起进程。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { [weak self] in
            guard let self = self, self.isActive else { return }
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
            // 交互圈选 + 无快门声 + 写临时 PNG；不用 -c，避免取消时清空剪贴板
            p.arguments = ["-i", "-x", "-t", "png", url.path]
            p.terminationHandler = { [weak self] proc in
                DispatchQueue.main.async {
                    self?.captureFinished(status: proc.terminationStatus, url: url)
                }
            }
            do {
                try p.run()
                self.process = p
            } catch {
                self.finish(copied: false)
            }
        }
    }

    private func captureFinished(status: Int32, url: URL) {
        process = nil
        defer { try? FileManager.default.removeItem(at: url) }
        guard status == 0,
              let data = try? Data(contentsOf: url),
              let rep = NSBitmapImageRep(data: data),
              rep.pixelsWide > 0, rep.pixelsHigh > 0 else {
            finish(copied: false)
            return
        }
        showEditor(with: rep)
    }

    private func showEditor(with rep: NSBitmapImageRep) {
        let pw = CGFloat(rep.pixelsWide), ph = CGFloat(rep.pixelsHigh)
        guard let screen = NSScreen.main?.visibleFrame else { finish(copied: false); return }
        let maxW = screen.width * 0.85
        let maxH = screen.height * 0.80 - kToolbarH
        let scale = min(1.0, min(maxW / pw, maxH / ph))
        let dispW = max(1, floor(pw * scale))
        let canvasH = max(1, floor(ph * scale))
        let winW = dispW, winH = canvasH + kToolbarH

        let win = KeyableWindow(
            contentRect: NSRect(x: 0, y: 0, width: winW, height: winH),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        win.isReleasedWhenClosed = false
        win.level = NSWindow.Level(rawValue: 100)
        win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        win.backgroundColor = .windowBackgroundColor
        win.isOpaque = true
        win.hasShadow = true
        win.setFrameOrigin(NSPoint(x: screen.minX + (screen.width - winW) / 2,
                                   y: screen.minY + (screen.height - winH) / 2))

        let view = AnnotationView(
            frame: NSRect(x: 0, y: 0, width: winW, height: winH),
            rep: rep, scale: scale, canvasHeight: canvasH
        )
        view.onFinish = { [weak self, weak view] copied in
            guard let self, let view else { return }
            var ok = false
            if copied, let png = view.renderPNG() {
                let pb = NSPasteboard.general
                pb.clearContents()
                ok = pb.setData(png, forType: .png)
            }
            self.finish(copied: ok)
        }
        win.contentView = view
        editor = win
        NSApp.activate(ignoringOtherApps: true)   // accessory 应用不激活会收不到中文 IME / 键盘
        win.makeKeyAndOrderFront(nil)
        win.makeFirstResponder(view)
    }

    private func finish(copied: Bool) {
        isActive = false
        if let win = editor {
            win.orderOut(nil)
            editor = nil
        }
        process?.terminate()
        process = nil
        if let url = tempURL { try? FileManager.default.removeItem(at: url) }
        tempURL = nil
        let cb = restore
        restore = nil
        let app = frontApp
        frontApp = nil
        cb?(noteWasVisible, ballWasVisible, copied)
        if let app, app != NSRunningApplication.current {
            app.activate(options: [.activateIgnoringOtherApps])
        }
    }
}
