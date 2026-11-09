#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GSticky Windows 图标生成器 —— 从项目根目录的 icon.png 生成 win/GSticky.ico。

设计要点：
  * 双平台共用唯一图标源 icon.png（macOS 版 build.sh 也用它），避免 Windows 版另画一套。
  * 零第三方依赖：只用 Python 标准库（struct / zlib），不需要 Pillow，不装任何软件。
  * 白底去背规则与 macOS 版 macos/tools/make_menubar_icon.swift 完全一致（近白判透明），
    保证两个平台图标观感一致；缩小时用带 alpha 预乘的面积平均，天然抗锯齿。
  * 输出标准 32bpp DIB 帧 ICO（不是 PNG 帧），windres / Windows 资源加载器兼容性最好。
    <=64px 用 DIB 保证托盘/任务栏兼容；>=128px 用 PNG 帧压缩（Vista+ 支持），
    否则 256px 单帧就要 256KB，会把单 exe 体积翻倍。

用法（使用 PATH 中的系统 Python）：
    python win/make_icon.py

生成结果 win/GSticky.ico 已随源码提交，日常构建 build.bat 不需要 Python；
只有更换 icon.png 时才需要重新跑本脚本。
"""

import os
import struct
import sys
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
WHITE_THRESHOLD = 230  # min(r,g,b) >= 230 视为白底，与 Swift 版 r/g/b >= 0.90 对齐
PNG_FROM_SIZE = 128    # 该尺寸及以上用 PNG 帧，小尺寸保留 DIB 帧


# ---- PNG 解码（8bit、非隔行；colortype 0/2/3/4/6）--------------------------------
def read_png_rgba(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)

    pos, idat, plte, trns = 8, bytearray(), None, None
    w = h = bd = ct = inter = None
    while pos + 8 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", chunk[:13])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"PLTE":
            plte = chunk
        elif typ == b"tRNS":
            trns = chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln

    if bd != 8 or inter != 0:
        raise ValueError("only 8-bit non-interlaced PNG is supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ct)
    if channels is None:
        raise ValueError("unsupported PNG color type %s" % ct)

    raw = zlib.decompress(bytes(idat))
    stride = w * channels
    if len(raw) < h * (stride + 1):
        raise ValueError("truncated PNG data")

    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError("unknown PNG filter %d" % ft)

        base = y * w * 4
        for x in range(w):
            s = x * channels
            if ct == 6:
                r, g, b, a = line[s], line[s + 1], line[s + 2], line[s + 3]
            elif ct == 2:
                r, g, b, a = line[s], line[s + 1], line[s + 2], 255
            elif ct == 3:
                idx = line[s]
                if plte is None or idx * 3 + 3 > len(plte):
                    raise ValueError("bad palette index")
                r, g, b = plte[idx * 3], plte[idx * 3 + 1], plte[idx * 3 + 2]
                a = trns[idx] if trns and idx < len(trns) else 255
            elif ct == 0:
                r = g = b = line[s]
                a = 255
            else:  # ct == 4
                r = g = b = line[s]
                a = line[s + 1]
            o = base + x * 4
            out[o], out[o + 1], out[o + 2], out[o + 3] = r, g, b, a
        prev = line
    return w, h, out


# ---- 白底去背 / 缩放 -----------------------------------------------------------
def dewhite(px, w, h):
    """与 macOS 版 make_menubar_icon.swift 一致：近白不透明像素转透明。

    若源图本身已带透明背景（透明像素占比 > 5%），则不再处理，避免二次破坏。
    """
    transparent = sum(1 for i in range(3, len(px), 4) if px[i] == 0)
    if transparent > w * h * 0.05:
        return px, False
    out = bytearray(px)
    for i in range(0, len(out), 4):
        r, g, b, a = out[i], out[i + 1], out[i + 2], out[i + 3]
        if a and min(r, g, b) >= WHITE_THRESHOLD:
            out[i + 3] = 0
    return out, True


def resize(sw, sh, src, dw, dh):
    """面积平均缩小 / 放大，颜色按 alpha 预乘加权，alpha 单独平均。"""
    out = bytearray(dw * dh * 4)
    for dy in range(dh):
        y0 = int(dy * sh / dh)
        y1 = min(sh, max(y0 + 1, int((dy + 1) * sh / dh)))
        for dx in range(dw):
            x0 = int(dx * sw / dw)
            x1 = min(sw, max(x0 + 1, int((dx + 1) * sw / dw)))
            ar = ag = ab = aa = cnt = 0
            for yy in range(y0, y1):
                base = yy * sw * 4
                for xx in range(x0, x1):
                    i = base + xx * 4
                    a = src[i + 3]
                    ar += src[i] * a
                    ag += src[i + 1] * a
                    ab += src[i + 2] * a
                    aa += a
                    cnt += 1
            j = (dy * dw + dx) * 4
            if aa > 0:
                out[j] = min(255, ar // aa)
                out[j + 1] = min(255, ag // aa)
                out[j + 2] = min(255, ab // aa)
            out[j + 3] = aa // cnt if cnt else 0
    return out


# ---- ICO 编码（32bpp DIB 帧）---------------------------------------------------
def dib_frame(w, h, rgba):
    xor = bytearray(w * h * 4)
    for y in range(h):
        sy = h - 1 - y  # DIB 自下而上
        for x in range(w):
            si, di = (sy * w + x) * 4, (y * w + x) * 4
            r, g, b, a = rgba[si], rgba[si + 1], rgba[si + 2], rgba[si + 3]
            xor[di], xor[di + 1], xor[di + 2], xor[di + 3] = b, g, r, a
    mask_stride = ((w + 31) // 32) * 4
    and_mask = bytearray(mask_stride * h)  # 32bpp 有 alpha，mask 全 0
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                      len(xor) + len(and_mask), 0, 0, 0, 0)
    return bytes(hdr) + bytes(xor) + bytes(and_mask)


def png_chunk(typ, payload):
    return (struct.pack(">I", len(payload)) + typ + payload +
            struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF))


def png_frame(w, h, rgba):
    scan = bytearray()
    for y in range(h):
        scan.append(0)  # filter type 0 (None)
        scan += rgba[y * w * 4:(y + 1) * w * 4]
    return (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(bytes(scan), 9)) +
            png_chunk(b"IEND", b""))


def frame_for(size, rgba):
    return (png_frame(size, size, rgba) if size >= PNG_FROM_SIZE
            else dib_frame(size, size, rgba))


def build_ico(frames):
    ico = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for size, data in frames:
        b = 0 if size == 256 else size  # ICO 目录里 0 表示 256
        ico += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in frames:
        ico += data
    return bytes(ico)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src_png = os.path.normpath(os.path.join(here, "..", "icon.png"))
    out_ico = os.path.join(here, "GSticky.ico")
    w, h, px = read_png_rgba(src_png)
    px, keyed = dewhite(px, w, h)
    frames = [(s, frame_for(s, resize(w, h, px, s, s))) for s in SIZES]
    with open(out_ico, "wb") as f:
        f.write(build_ico(frames))
    print("source      : %s (%dx%d, RGBA)" % (src_png, w, h))
    print("white-keyed : %s" % ("yes" if keyed else "no (source already has alpha)"))
    print("output      : %s" % out_ico)
    print("sizes       : %s" % ", ".join(str(s) for s in SIZES))
    print("bytes       : %d" % os.path.getsize(out_ico))
    return 0


if __name__ == "__main__":
    sys.exit(main())
