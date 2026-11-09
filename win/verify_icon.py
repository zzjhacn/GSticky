#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 dist/GSticky.exe 的图标资源是否正确嵌入并可被 Windows 加载。

检查项：
  1. exe 内 RT_GROUP_ICON = 1 组、RT_ICON = 9 帧（16/20/24/32/40/48/64/128/256）
  2. 32x32（DIB 帧）与 256x256（PNG 帧）都能被系统加载且含非透明像素
  3. 导出 256x256 预览 PNG 到系统临时目录，便于肉眼核对图案与透明背景

用法（使用 PATH 中的系统 Python）：
    python win/verify_icon.py
"""

import ctypes
import json
import os
import struct
import sys
import tempfile
import zlib
from ctypes import wintypes as w

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.normpath(os.path.join(HERE, "..", "dist", "GSticky.exe"))

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
s32 = ctypes.WinDLL("shell32", use_last_error=True)

RT_ICON, RT_GROUP_ICON = 3, 14
LOAD_LIBRARY_AS_DATAFILE = 0x2
IMAGE_ICON = 1


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", w.BOOL), ("xHotspot", w.DWORD), ("yHotspot", w.DWORD),
                ("hbmMask", w.HBITMAP), ("hbmColor", w.HBITMAP)]


class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                ("bmPlanes", w.WORD), ("bmBitsPixel", w.WORD), ("bmBits", ctypes.c_void_p)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", w.WORD),
                ("biBitCount", w.WORD), ("biCompression", w.DWORD),
                ("biSizeImage", w.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", w.DWORD),
                ("biClrImportant", w.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", w.DWORD * 3)]


# 资源名既可能是整数 ID 也可能是字符串，统一用 void* 接收，避免误当指针解引用
ENUMRESNAMEPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HMODULE, ctypes.c_void_p,
                                     ctypes.c_void_p, w.LPARAM)
k32.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
k32.LoadLibraryExW.restype = w.HMODULE
k32.EnumResourceNamesW.argtypes = [w.HMODULE, ctypes.c_void_p, ENUMRESNAMEPROC, w.LPARAM]
k32.EnumResourceNamesW.restype = w.BOOL
k32.FreeLibrary.argtypes = [w.HMODULE]

u32.GetIconInfo.argtypes = [w.HICON, ctypes.POINTER(ICONINFO)]
u32.GetIconInfo.restype = w.BOOL
u32.GetDC.argtypes = [w.HWND]
u32.GetDC.restype = w.HDC
u32.ReleaseDC.argtypes = [w.HWND, w.HDC]
u32.DestroyIcon.argtypes = [w.HICON]
g32.GetObjectW.argtypes = [w.HGDIOBJ, ctypes.c_int, ctypes.c_void_p]
g32.GetObjectW.restype = ctypes.c_int
g32.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p,
                          ctypes.POINTER(BITMAPINFO), w.UINT]
g32.GetDIBits.restype = ctypes.c_int
s32.SHDefExtractIconW.argtypes = [w.LPCWSTR, ctypes.c_int, w.UINT,
                                  ctypes.POINTER(w.HICON), ctypes.POINTER(w.HICON), w.UINT]
s32.SHDefExtractIconW.restype = ctypes.c_long


def count_resources(hmod, res_type):
    found = []

    @ENUMRESNAMEPROC
    def cb(module, typ, name, param):
        found.append(name)
        return True

    k32.EnumResourceNamesW(hmod, ctypes.c_void_p(res_type), cb, 0)
    return len(found)


def icon_pixels(hicon):
    """把 HICON 的 32bpp 颜色位图读成 RGBA 字节。"""
    ii = ICONINFO()
    if not u32.GetIconInfo(hicon, ctypes.byref(ii)) or not ii.hbmColor:
        return 0, 0, bytearray()
    bm = BITMAP()
    g32.GetObjectW(ii.hbmColor, ctypes.sizeof(BITMAP), ctypes.byref(bm))
    width, height = bm.bmWidth, abs(bm.bmHeight)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = width
    bi.bmiHeader.biHeight = -height  # top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    buf = (ctypes.c_ubyte * (width * height * 4))()
    dc = u32.GetDC(None)
    got = g32.GetDIBits(dc, ii.hbmColor, 0, height, buf, ctypes.byref(bi), 0)
    u32.ReleaseDC(None, dc)
    if not got:
        return 0, 0, bytearray()
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        b, g, r, a = buf[i * 4:i * 4 + 4]
        rgba[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return width, height, rgba


def png_chunk(typ, payload):
    return (struct.pack(">I", len(payload)) + typ + payload +
            struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF))


def write_png(path, width, height, rgba):
    scan = bytearray()
    for y in range(height):
        scan.append(0)  # filter None
        scan += rgba[y * width * 4:(y + 1) * width * 4]
    data = (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(bytes(scan), 9)) +
            png_chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


def main():
    result = {"exe": EXE, "exists": os.path.isfile(EXE)}
    if not result["exists"]:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 1

    hmod = k32.LoadLibraryExW(EXE, None, LOAD_LIBRARY_AS_DATAFILE)
    result["rt_icon_frames"] = count_resources(hmod, RT_ICON) if hmod else 0
    result["rt_group_icon"] = count_resources(hmod, RT_GROUP_ICON) if hmod else 0
    if hmod:
        k32.FreeLibrary(hmod)

    for size in (32, 256):
        large, small = w.HICON(), w.HICON()
        hr = s32.SHDefExtractIconW(EXE, 0, 0, ctypes.byref(large),
                                   ctypes.byref(small), (size << 16) | size)
        width = height = 0
        opaque = 0
        if large:
            width, height, rgba = icon_pixels(large)
            opaque = sum(1 for i in range(3, len(rgba), 4) if rgba[i] > 0)
            if size == 256 and width and height:
                preview = os.path.join(tempfile.gettempdir(), "gsticky_exe_icon_256.png")
                write_png(preview, width, height, rgba)
                result["preview_256"] = preview
        result["icon_%d" % size] = {
            "hr": hex(hr & 0xFFFFFFFF), "loaded": bool(large),
            "actual_size": [width, height], "opaque_pixels": opaque,
        }
        if large:
            u32.DestroyIcon(large)
        if small:
            u32.DestroyIcon(small)

    result["ok"] = (result["rt_group_icon"] == 1 and
                    result["rt_icon_frames"] == 9 and
                    all(result["icon_%d" % s]["loaded"] and
                        result["icon_%d" % s]["opaque_pixels"] > 0 for s in (32, 256)))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
