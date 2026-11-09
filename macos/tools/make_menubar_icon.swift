import AppKit

// 用法: swift make_menubar_icon.swift <输入png> <输出png>
// 作用: 把纯白背景转透明、按不透明内容裁边、居中留白，生成适合菜单栏的小图标。
let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: make_menubar_icon <in.png> <out.png>\n".data(using: .utf8)!)
    exit(1)
}
let inPath = args[1]
let outPath = args[2]

guard let img = NSImage(contentsOfFile: inPath),
      let tiff = img.tiffRepresentation,
      let src = NSBitmapImageRep(data: tiff) else {
    FileHandle.standardError.write("load fail: \(inPath)\n".data(using: .utf8)!)
    exit(1)
}

let w = src.pixelsWide
let h = src.pixelsHigh
guard let keyed = NSBitmapImageRep(
    bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
) else { exit(1) }

var minX = w, minY = h, maxX = -1, maxY = -1
for y in 0..<h {
    for x in 0..<w {
        guard let c = src.colorAt(x: x, y: y) else { continue }
        let r = c.redComponent, g = c.greenComponent, b = c.blueComponent
        // 近白（含抗锯齿边缘的浅色）判为背景
        let isWhite = (r >= 0.90 && g >= 0.90 && b >= 0.90)
        let a: CGFloat = isWhite ? 0 : 1
        keyed.setColor(NSColor(calibratedRed: r, green: g, blue: b, alpha: a), atX: x, y: y)
        if !isWhite {
            if x < minX { minX = x }
            if x > maxX { maxX = x }
            if y < minY { minY = y }
            if y > maxY { maxY = y }
        }
    }
}

guard maxX >= 0 else {
    FileHandle.standardError.write("all pixels are white\n".data(using: .utf8)!)
    exit(1)
}

let cw = maxX - minX + 1
let ch = maxY - minY + 1
let side = max(cw, ch)
let pad = max(1, Int(Double(side) * 0.08))
let canvas = side + pad * 2

guard let final = NSBitmapImageRep(
    bitmapDataPlanes: nil, pixelsWide: canvas, pixelsHigh: canvas,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
) else { exit(1) }

let ox = (canvas - cw) / 2
let oy = (canvas - ch) / 2
for y in 0..<ch {
    for x in 0..<cw {
        if let c = keyed.colorAt(x: minX + x, y: minY + y) {
            final.setColor(c, atX: ox + x, y: oy + y)
        }
    }
}

guard let png = final.representation(using: .png, properties: [:]) else { exit(1) }
do {
    try png.write(to: URL(fileURLWithPath: outPath))
    print("menubar icon \(canvas)x\(canvas) -> \(outPath)")
} catch {
    FileHandle.standardError.write("write fail: \(error)\n".data(using: .utf8)!)
    exit(1)
}
