# -*- coding: utf-8 -*-
"""GSticky.exe 端到端验证（真实二进制，不是模拟窗口）。

验证 dist/GSticky.exe 这个真实产品的「行为」是否达标，与 exe 内建
--selftest（只查窗口/几何/层级等静态状态）互补。

运行：  python win/verify_exe.py           （使用 PATH 中的系统 Python）
过程：  启动 exe → 向 Edit 写入中文/换行/缩进 → 等 0.5s debounce 落盘 →
        触发「复制全部」→ 模拟热键显隐 → 抓悬浮球屏幕画面做像素断言 →
        检查悬浮球互斥 / 不抢焦点 → 走托盘退出命令。
副作用控制：
  * 便签与托盘图标会短暂出现约 3 秒，结束后自动退出；
  * note.txt / config.ini 先备份、验证后原样恢复（原本不存在则删除）；
  * 剪贴板若含非文本格式则跳过复制检查，否则验证后恢复原文本；
  * 开机自启开关检查默认跳过；--autostart 会短暂改写用户真实的 Run 项，
    但无论成败都在 finally 中原样还原。
  * 几何记忆检查默认跳过；--geometry 会重开两次 exe 并改写 config.ini / note.txt，
    同样在 finally 中把两者原样还原（原本不存在则删除）。
"""
import ctypes
import json
import os
import subprocess
import sys
import tempfile
import time
import winreg
from ctypes import wintypes as w

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "dist", "GSticky.exe")
DATA = os.path.join(os.environ.get("APPDATA", ""), "GSticky")
NOTE = os.path.join(DATA, "note.txt")
CONFIG = os.path.join(DATA, "config.ini")
NOTE_CLASS = "GStickyNoteWindow_v1"
BALL_CLASS = "GStickyBallWindow_v1"

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# 与 exe 统一为 DPI-aware，否则 GetWindowRect/工作区会被系统虚拟化成逻辑像素
try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass

WPARAM = ctypes.c_size_t
LPARAM = ctypes.c_ssize_t
u32.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
u32.FindWindowW.restype = w.HWND
u32.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u32.FindWindowExW.restype = w.HWND
u32.IsWindowVisible.argtypes = [w.HWND]
u32.IsWindowVisible.restype = w.BOOL
u32.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
u32.GetWindowLongPtrW.argtypes = [w.HWND, ctypes.c_int]
u32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
u32.SendMessageW.argtypes = [w.HWND, w.UINT, WPARAM, LPARAM]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.PostMessageW.argtypes = [w.HWND, w.UINT, WPARAM, LPARAM]
u32.PostMessageW.restype = w.BOOL
u32.SystemParametersInfoW.argtypes = [w.UINT, w.UINT, ctypes.POINTER(w.RECT), w.UINT]
u32.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, w.UINT]
u32.SetWindowPos.restype = w.BOOL
u32.GetForegroundWindow.restype = w.HWND
u32.OpenClipboard.argtypes = [w.HWND]
u32.OpenClipboard.restype = w.BOOL
u32.CloseClipboard.argtypes = []
u32.CloseClipboard.restype = w.BOOL
u32.EmptyClipboard.argtypes = []
u32.EmptyClipboard.restype = w.BOOL
u32.GetClipboardData.argtypes = [w.UINT]
u32.GetClipboardData.restype = w.HGLOBAL
u32.SetClipboardData.argtypes = [w.UINT, w.HGLOBAL]
u32.SetClipboardData.restype = w.HGLOBAL
u32.EnumClipboardFormats.argtypes = [w.UINT]
u32.EnumClipboardFormats.restype = w.UINT
k32.GlobalAlloc.argtypes = [w.UINT, ctypes.c_size_t]
k32.GlobalAlloc.restype = w.HGLOBAL
k32.GlobalLock.argtypes = [w.HGLOBAL]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalUnlock.argtypes = [w.HGLOBAL]
k32.GlobalUnlock.restype = w.BOOL
k32.GlobalFree.argtypes = [w.HGLOBAL]
k32.GlobalFree.restype = w.HGLOBAL

WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_CHAR = 0x0102
WM_COMMAND = 0x0111
WM_HOTKEY = 0x0312
WM_EXITSIZEMOVE = 0x0232
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
ID_COPY_ALL = 4002
ID_QUIT = 4003
ID_AUTOSTART = 4004
HOTKEY_ID = 1
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
SPI_GETWORKAREA = 0x0030
CF_UNICODETEXT = 13
TEXT_FORMATS = (1, 7, 13, 16)  # CF_TEXT / OEMTEXT / UNICODETEXT / LOCALE
GMEM_MOVEABLE = 0x0002


def clip_formats():
    if not u32.OpenClipboard(None):
        return None
    out, fmt = [], 0
    while True:
        fmt = u32.EnumClipboardFormats(fmt)
        if not fmt:
            break
        out.append(fmt)
    u32.CloseClipboard()
    return out


def clip_text():
    if not u32.OpenClipboard(None):
        return None
    h = u32.GetClipboardData(CF_UNICODETEXT)
    text = None
    if h:
        p = k32.GlobalLock(h)
        if p:
            text = ctypes.wstring_at(p)
            k32.GlobalUnlock(h)
    u32.CloseClipboard()
    return text


def set_clip_text(text):
    if not u32.OpenClipboard(None):
        return False
    u32.EmptyClipboard()
    ok = False
    if text is not None:
        buf = ctypes.create_unicode_buffer(text)
        size = ctypes.sizeof(buf)
        h = k32.GlobalAlloc(GMEM_MOVEABLE, size)
        p = k32.GlobalLock(h)
        ctypes.memmove(p, buf, size)
        k32.GlobalUnlock(h)
        ok = bool(u32.SetClipboardData(CF_UNICODETEXT, h))
        if not ok:
            k32.GlobalFree(h)
    u32.CloseClipboard()
    return ok


# ---- 开机自启开关（可选端到端检查） -----------------------------------------
# 真实发两次托盘命令，验证注册表值的写入与删除、命令格式与 exe 路径。
# 会短暂改写用户真实的 Run 项，因此默认跳过；无论如何 finally 中原样还原。
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "GSticky"


def read_run_value():
    """返回 (data, type)；键或值不存在返回 None。"""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_QUERY_VALUE) as k:
            return winreg.QueryValueEx(k, RUN_VALUE)
    except FileNotFoundError:
        return None


def restore_run_value(state):
    """把 Run 项恢复成 state（None 表示删除该值）。"""
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as k:
        if state is None:
            try:
                winreg.DeleteValue(k, RUN_VALUE)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(k, RUN_VALUE, 0, state[1], state[0])


def check_autostart(note):
    result = {}
    before = read_run_value()
    try:
        u32.PostMessageW(note, WM_COMMAND, ID_AUTOSTART, 0)
        time.sleep(0.35)
        mid = read_run_value()
        u32.PostMessageW(note, WM_COMMAND, ID_AUTOSTART, 0)
        time.sleep(0.35)
        after = read_run_value()

        result["autostart_toggle_flips_state"] = (mid is None) != (before is None)
        # 只校验「开关状态」回到原点：重新写入时 exe 用当前进程的模块路径，
        # 盘符大小写可能与原值不同（功能等价），不必要求字节级相等。
        # 字节级原样还原由 finally 的 autostart_state_restored 负责。
        result["autostart_toggle_restores"] = (after is None) == (before is None)
        # 两次快照里必有一个处于「开启」状态，用它核对命令格式与 exe 路径
        enabled = mid if mid is not None else after
        cmd = enabled[0] if enabled else ""
        result["autostart_command_quoted"] = (
            isinstance(cmd, str) and cmd.startswith('"') and cmd.endswith('"'))
        result["autostart_command_matches_exe"] = (
            isinstance(cmd, str) and
            os.path.normcase(os.path.normpath(cmd.strip('"'))) ==
            os.path.normcase(os.path.normpath(EXE)))
        result["autostart_run_value_type"] = enabled[1] if enabled else None
    finally:
        try:
            restore_run_value(before)
        except OSError as exc:
            result["autostart_restore_error"] = str(exc)
        result["autostart_state_restored"] = (read_run_value() == before)
    return result


# ---- 便签几何记忆（可选端到端检查） -----------------------------------------
# 真实改窗口 → 触发 WM_EXITSIZEMOVE 保存 → 退出 → 重启 → 断言大小与位置恢复。
# 会改写用户真实的 config.ini 与 note.txt，默认跳过；finally 中原样还原。
def _ini_text(path):
    raw = open(path, "rb").read()
    # WritePrivateProfileStringW 新建文件时可能是 ANSI，也可能是 UTF-16 BOM
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    return raw.decode("latin-1", errors="replace")


def _launch_app():
    time.sleep(0.20)  # 等上一实例的窗口彻底销毁，避免 FindWindow 命中旧窗口
    p = subprocess.Popen([EXE])
    note = 0
    for _ in range(60):
        note = u32.FindWindowW(NOTE_CLASS, None)
        if note:
            break
        time.sleep(0.05)
    time.sleep(0.35)
    return p, note


def _quit_app(note, p):
    u32.PostMessageW(note, WM_COMMAND, ID_QUIT, 0)
    try:
        return p.wait(timeout=6)
    except subprocess.TimeoutExpired:
        p.kill()
        return -1


def check_geometry():
    result = {}
    cfg_backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as f:
            cfg_backup = f.read()
    note_backup = None
    if os.path.exists(NOTE):
        with open(NOTE, "rb") as f:
            note_backup = f.read()
    p1 = p2 = None
    try:
        p1, note1 = _launch_app()
        result["geometry_first_launch"] = bool(note1) and p1.poll() is None
        if not note1:
            return result
        wa = w.RECT()
        u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(wa), 0)
        nx, ny = wa.left + 300, wa.top + 240
        nw, nh = 500, 380
        u32.SetWindowPos(note1, None, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE)
        time.sleep(0.25)
        r1 = w.RECT()
        u32.GetWindowRect(note1, ctypes.byref(r1))
        result["geometry_applied"] = (
            abs(r1.left - nx) <= 2 and abs(r1.top - ny) <= 2 and
            abs((r1.right - r1.left) - nw) <= 2 and
            abs((r1.bottom - r1.top) - nh) <= 2)
        u32.PostMessageW(note1, WM_EXITSIZEMOVE, 0, 0)
        time.sleep(0.30)
        txt = _ini_text(CONFIG) if os.path.exists(CONFIG) else ""
        result["geometry_config_written"] = "[note]" in txt and "rect=" in txt
        rc1 = _quit_app(note1, p1)
        p1 = None
        result["geometry_first_exit"] = (rc1 == 0)

        p2, note2 = _launch_app()
        result["geometry_second_launch"] = bool(note2) and p2.poll() is None
        if note2:
            r2 = w.RECT()
            u32.GetWindowRect(note2, ctypes.byref(r2))
            result["geometry_restored_rect"] = [r2.left, r2.top, r2.right, r2.bottom]
            # 容差覆盖 DPI 逻辑像素往返的 ±1 与 DWM 不可见边框
            result["geometry_restored_position"] = (
                abs(r2.left - nx) <= 12 and abs(r2.top - ny) <= 12)
            result["geometry_restored_size"] = (
                abs((r2.right - r2.left) - nw) <= 12 and
                abs((r2.bottom - r2.top) - nh) <= 12)
            rc2 = _quit_app(note2, p2)
            p2 = None
            result["geometry_second_exit"] = (rc2 == 0)
    finally:
        for p in (p1, p2):
            if p is not None and p.poll() is None:
                p.kill()
        for path, backup in ((CONFIG, cfg_backup), (NOTE, note_backup)):
            if backup is not None:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(backup)
            else:
                try:
                    os.remove(path)
                except OSError:
                    pass
        result["geometry_config_restored"] = (
            (not os.path.exists(CONFIG)) if cfg_backup is None
            else (os.path.exists(CONFIG) and open(CONFIG, "rb").read() == cfg_backup))
    return result


# ---- 悬浮球外观实证 ---------------------------------------------------------
# 只验证「图标句柄来自资源」不够：DrawIconEx 的 alpha 混合、圆底颜色、居中位置
# 都要看真实合成画面。这里用屏幕 BitBlt 抓球窗口区域，做像素级断言并导出预览。
def capture_ball_pixels(ball):
    import struct  # noqa: F401  局部导入，保持主流程轻量
    g32 = ctypes.WinDLL("gdi32", use_last_error=True)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", w.DWORD), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", w.WORD),
                    ("biBitCount", w.WORD), ("biCompression", w.DWORD),
                    ("biSizeImage", w.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", w.DWORD),
                    ("biClrImportant", w.DWORD)]

    class BInfo(ctypes.Structure):
        _fields_ = [("bmiHeader", BIH), ("bmiColors", w.DWORD * 3)]

    u32.GetDC.argtypes = [w.HWND]
    u32.GetDC.restype = w.HDC
    u32.ReleaseDC.argtypes = [w.HWND, w.HDC]
    u32.ReleaseDC.restype = ctypes.c_int
    g32.CreateCompatibleDC.argtypes = [w.HDC]
    g32.CreateCompatibleDC.restype = w.HDC
    g32.CreateDIBSection.argtypes = [w.HDC, ctypes.POINTER(BInfo), w.UINT,
                                     ctypes.POINTER(ctypes.c_void_p), w.HANDLE, w.DWORD]
    g32.CreateDIBSection.restype = w.HBITMAP
    g32.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
    g32.SelectObject.restype = w.HGDIOBJ
    g32.BitBlt.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                           ctypes.c_int, w.HDC, ctypes.c_int, ctypes.c_int, w.DWORD]
    g32.BitBlt.restype = w.BOOL
    g32.DeleteObject.argtypes = [w.HGDIOBJ]
    g32.DeleteObject.restype = w.BOOL
    g32.DeleteDC.argtypes = [w.HDC]
    g32.DeleteDC.restype = w.BOOL
    u32.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
    u32.PrintWindow.restype = w.BOOL

    r = w.RECT()
    u32.GetWindowRect(ball, ctypes.byref(r))
    W, H = r.right - r.left, r.bottom - r.top
    if W <= 0 or H <= 0:
        return None
    screen = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(screen)
    bi = BInfo()
    bi.bmiHeader.biSize = ctypes.sizeof(BIH)
    bi.bmiHeader.biWidth = W
    bi.bmiHeader.biHeight = -H      # top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0  # BI_RGB
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(mem, ctypes.byref(bi), 0,
                               ctypes.byref(bits), None, 0)
    if not bmp:
        g32.DeleteDC(mem)
        u32.ReleaseDC(None, screen)
        return None
    old = g32.SelectObject(mem, bmp)
    # 清零，避免抓取失败时 DIB 里的随机像素被当成有效画面
    ctypes.memset(bits, 0, W * H * 4)
    # 优先 PrintWindow：layered 窗口由 DWM 独立合成，GetDC(NULL)+BitBlt
    # 常常只抓到桌面壁纸；PrintWindow 抓的是窗口自身的绘制结果。
    method = None
    data = None
    if u32.PrintWindow(ball, mem, 2):  # PW_RENDERFULLCONTENT
        method = "PrintWindow"
        data = ctypes.string_at(bits, W * H * 4)
    elif g32.BitBlt(mem, 0, 0, W, H, screen, r.left, r.top, 0x00CC0020):
        method = "screen BitBlt (需经圆底校验)"
        data = ctypes.string_at(bits, W * H * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(None, screen)
    return (W, H, data, method) if data else None


def ball_visual_checks(ball, out_png):
    """只统计椭圆内部：品牌青/绿像素应存在，旧黄底与深色方块应消失。"""
    import struct
    import zlib
    cap = capture_ball_pixels(ball)
    if not cap:
        return {"ball_capture": "failed (BitBlt 不可用)"}
    W, H, data, method = cap
    cx, cy = W / 2.0, H / 2.0
    rad = min(W, H) * 0.42
    total = brand = yellow = dark = near_white = 0
    for y in range(H):
        for x in range(W):
            if (x - cx) ** 2 + (y - cy) ** 2 > rad * rad:
                continue
            i = (y * W + x) * 4
            b, g, r = data[i], data[i + 1], data[i + 2]  # BGRA
            total += 1
            if min(r, g, b) > 200:                          # 系统色圆底
                near_white += 1
            # icon.png 的绿 (≈140,200,40) 与青 (≈70,170,190)
            if ((g > r + 25 and g > b + 30 and g > 110) or
                    (b > r + 30 and g > r + 20 and g > 100 and b > 110)):
                brand += 1
            if r > 210 and 170 < g < 245 and b < 175:      # 旧自绘黄底
                yellow += 1
            if max(r, g, b) < 90:                           # 旧自绘深色方块 / alpha 失败
                dark += 1
    # 有效性门槛：必须以「浅色圆底」为主，否则抓到的只是桌面壁纸（蓝色也会
    # 被误判成青色品牌像素），不能据此下结论。
    valid = total > 0 and near_white >= int(total * 0.25)
    try:
        def chunk(t, d):
            return (struct.pack(">I", len(d)) + t + d +
                    struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))
        scale = 4
        outer = (min(W, H) / 2.0) ** 2
        scan = bytearray()
        for y in range(H):
            row = bytearray()
            for x in range(W):
                i = (y * W + x) * 4
                r_, g_, b_ = data[i + 2], data[i + 1], data[i]
                outside = (x - cx) ** 2 + (y - cy) ** 2 > outer
                # 绘制代码只画白圆 + 彩色 logo，从不画黑色；近黑像素即 DIB
                # 未填充区。与 RGN 裁掉的圆外区域一起渲染成棋盘格表示透明。
                if outside or (r_ < 30 and g_ < 30 and b_ < 30):
                    c = 255 if ((x * scale // 8 + y * scale // 8) % 2 == 0) else 210
                    r_ = g_ = b_ = c
                row += bytes((r_, g_, b_, 255)) * scale
            for _ in range(scale):
                scan.append(0)  # PNG filter None
                scan += row
        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", W * scale, H * scale,
                                          8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(bytes(scan), 9)) +
               chunk(b"IEND", b""))
        with open(out_png, "wb") as f:
            f.write(png)
    except Exception as exc:
        out_png = "write failed: %s" % exc
    return {
        "ball_capture": method or "unknown",
        "ball_capture_valid": valid,
        "ball_size": [W, H],
        "ball_inside_pixels": total,
        "ball_near_white_pixels": near_white,
        "ball_brand_pixels": brand,
        "ball_yellow_pixels": yellow,
        "ball_dark_pixels": dark,
        "ball_uses_brand_logo": valid and brand >= max(40, int(total * 0.02)),
        "ball_no_legacy_yellow": valid and yellow <= int(total * 0.05),
        "ball_no_legacy_square": valid and dark <= int(total * 0.05),
        "ball_preview": out_png,
    }


def check_screenshot_build(exe_path=None):
    """只读 exe 字节，确认截图子系统确实被链接进产物。

    区域截图的圈选/标注必须人工交互，自动化会抢占全屏、隐藏浮层并改写
    剪贴板，与「不污染用户数据」约定冲突，所以只做构建完整性检查：
    不启动 exe、不建窗口、不碰便签/配置/剪贴板。
    """
    path = exe_path or EXE
    with open(path, "rb") as f:
        blob = f.read()
    needles = {
        "shot_overlay_class": "GStickyShotOverlay_v1",
        "shot_editor_class": "GStickyShotEditor_v1",
        "shot_menu_entry": "区域截图（可标注）",
    }
    out = {key: (text.encode("utf-16-le") in blob) for key, text in needles.items()}
    out["shot_build_present"] = all(out.values())
    return out


def main():
    if not os.path.exists(EXE):
        print(json.dumps({"ok": False, "error": "dist/GSticky.exe 不存在，请先构建"},
                         ensure_ascii=False, indent=2))
        return 1

    checks = {}
    checks.update(check_screenshot_build())
    existed = os.path.exists(NOTE)
    original = None
    if existed:
        with open(NOTE, "rb") as f:
            original = f.read()
    config_original = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as f:
            config_original = f.read()
    proc = None
    try:
        proc = subprocess.Popen([EXE])
        note = 0
        for _ in range(60):
            note = u32.FindWindowW(NOTE_CLASS, None)
            if note:
                break
            time.sleep(0.05)
        checks["process_started"] = bool(note) and proc.poll() is None
        if not note:
            print(json.dumps({"ok": False, "error": "未找到便签窗口",
                              "checks": checks}, ensure_ascii=False, indent=2))
            return 1
        time.sleep(0.30)

        checks["note_visible"] = bool(u32.IsWindowVisible(note))
        ex = u32.GetWindowLongPtrW(note, GWL_EXSTYLE)
        checks["note_topmost"] = bool(ex & WS_EX_TOPMOST)
        wa = w.RECT()
        u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(wa), 0)
        r = w.RECT()
        u32.GetWindowRect(note, ctypes.byref(r))
        checks["work_area"] = [wa.left, wa.top, wa.right, wa.bottom]
        checks["note_rect"] = [r.left, r.top, r.right, r.bottom]
        checks["note_inside_work_area"] = (
            r.left >= wa.left and r.top >= wa.top and
            r.right <= wa.right and r.bottom <= wa.bottom)
        edit = u32.FindWindowExW(note, 0, "Edit", None)
        checks["edit_found"] = bool(edit)

        sample = "GSticky E2E 中文\r\n第二行  缩进\tTab"
        expected_saved = "GSticky E2E 中文\n第二行  缩进\tTab"

        def read_note():
            if not os.path.exists(NOTE):
                return b""
            with open(NOTE, "rb") as f:
                return f.read()

        # A) 真实输入路径：WM_CHAR 由 Edit 控件处理，必然发出 EN_CHANGE →
        #    0.5s debounce → 原子写。这才是产品实际运行时走的路。
        empty = ctypes.create_unicode_buffer("")
        u32.SendMessageW(edit, WM_SETTEXT, 0, ctypes.addressof(empty))
        u32.SendMessageW(edit, WM_CHAR, ord("A"), 0)
        u32.SendMessageW(edit, WM_CHAR, ord("B"), 0)
        time.sleep(0.95)
        checks["real_input_autosave"] = b"AB" in read_note()

        # B) 完整内容精确落盘：跨进程 WM_SETTEXT 不触发 EN_CHANGE（系统
        #    marshalling 直接改文本），故补发一条标准 EN_CHANGE 通知。
        buf = ctypes.create_unicode_buffer(sample)
        set_rc = u32.SendMessageW(edit, WM_SETTEXT, 0, ctypes.addressof(buf))
        getbuf = ctypes.create_unicode_buffer(1024)
        got_n = u32.SendMessageW(edit, WM_GETTEXT, 1024, ctypes.addressof(getbuf))
        checks["settext_result"] = set_rc
        checks["edit_text_readback"] = getbuf.value
        checks["edit_text_set_ok"] = (getbuf.value == sample and got_n == len(sample))
        en_change = (0x0300 << 16) | 1  # MAKEWPARAM(控件 ID=1, EN_CHANGE)
        u32.PostMessageW(note, WM_COMMAND, en_change, edit)
        time.sleep(0.95)
        saved = read_note()
        checks["autosave_file_created"] = bool(saved)
        checks["utf8_no_bom"] = bool(saved) and not saved.startswith(b"\xef\xbb\xbf")
        checks["autosave_lf_only"] = bool(saved) and b"\r" not in saved
        try:
            checks["autosave_exact"] = saved.decode("utf-8") == expected_saved
        except UnicodeDecodeError:
            checks["autosave_exact"] = False
        checks["saved_bytes"] = len(saved)

        # 复制全部：只应写 CF_UNICODETEXT，不夹带富文本格式
        # 注意区分「剪贴板为空 []」与「打不开剪贴板 None」：后者一律跳过，
        # 否则会把「其它进程瞬时占用剪贴板」误记成产品复制失败。
        before = clip_formats()
        non_text = ([f for f in before if f not in TEXT_FORMATS]
                    if before is not None else None)
        if before is None:
            checks["clipboard_check"] = "skipped: 剪贴板被其它进程占用，无法读取"
        elif non_text and "--clipboard" not in sys.argv:
            checks["clipboard_check"] = "skipped: 原剪贴板含非文本格式，避免破坏用户数据（--clipboard 可强制）"
        else:
            orig_clip = clip_text()
            u32.PostMessageW(note, WM_COMMAND, ID_COPY_ALL, 0)
            time.sleep(0.30)
            after = clip_formats() or []
            copied = clip_text()
            checks["copy_all_clipboard_text"] = repr(copied)
            checks["copy_all_plain_text"] = copied == sample
            checks["copy_all_no_extra_format"] = all(f in TEXT_FORMATS for f in after)
            if orig_clip is not None:
                set_clip_text(orig_clip)

        # 热键显隐 + 悬浮球互斥 + 不抢焦点
        u32.PostMessageW(note, WM_HOTKEY, HOTKEY_ID, 0)  # 隐藏
        time.sleep(0.35)
        checks["hidden_by_hotkey"] = not bool(u32.IsWindowVisible(note))
        ball = u32.FindWindowW(BALL_CLASS, None)
        checks["ball_visible_when_hidden"] = bool(ball and u32.IsWindowVisible(ball))
        if ball:
            bex = u32.GetWindowLongPtrW(ball, GWL_EXSTYLE)
            checks["ball_noactivate"] = bool(bex & WS_EX_NOACTIVATE)
            time.sleep(0.20)  # 等 DWM 完成合成再抓图
            checks.update(ball_visual_checks(
                ball, os.path.join(tempfile.gettempdir(), "gsticky_ball_preview.png")))
        fg = u32.GetForegroundWindow()
        checks["foreground_not_stolen_by_ball"] = (fg != note and fg != ball)

        u32.PostMessageW(note, WM_HOTKEY, HOTKEY_ID, 0)  # 再显示
        time.sleep(0.35)
        checks["shown_by_hotkey"] = bool(u32.IsWindowVisible(note))
        checks["ball_hidden_when_shown"] = not bool(u32.IsWindowVisible(ball))

        # 开机自启开关：默认跳过，--autostart 时做「切两次 + 原样还原」的实证
        if "--autostart" in sys.argv:
            checks.update(check_autostart(note))
        else:
            checks["autostart_check"] = "skipped: 会短暂改写用户自启设置（--autostart 可强制）"

        # 走托盘「退出」命令，验证正常清理与退出码
        u32.PostMessageW(note, WM_COMMAND, ID_QUIT, 0)
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = -1
        checks["exit_code"] = rc
        checks["clean_exit"] = (rc == 0)
        # 退出时 SaveNow() 会再写一次：据此区分「文本没设进去」与「timer 没触发」
        tail = b""
        if os.path.exists(NOTE):
            with open(NOTE, "rb") as f:
                tail = f.read()
        checks["note_bytes_after_exit"] = len(tail)
        checks["note_content_after_exit"] = tail.decode("utf-8", "replace")
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        # 还原用户数据
        if original is not None:
            os.makedirs(DATA, exist_ok=True)
            with open(NOTE, "wb") as f:
                f.write(original)
        else:
            try:
                os.remove(NOTE)
            except OSError:
                pass
        # 还原窗口几何配置：exe 在 WM_DESTROY 时会自动写 [note]，
        # 任何一次验证启动都不能改动用户真实 config.ini
        if config_original is not None:
            os.makedirs(DATA, exist_ok=True)
            with open(CONFIG, "wb") as f:
                f.write(config_original)
        else:
            try:
                os.remove(CONFIG)
            except OSError:
                pass

    # 几何记忆：默认跳过（会重启 exe 并改写用户配置），--geometry 时实证
    if "--geometry" in sys.argv:
        checks.update(check_geometry())
    else:
        checks["geometry_check"] = "skipped: 会重启 exe 并改写用户配置（--geometry 可强制）"

    required = [
        "process_started", "note_visible", "note_topmost", "note_inside_work_area",
        "edit_found", "edit_text_set_ok", "real_input_autosave",
        "autosave_file_created", "utf8_no_bom", "autosave_lf_only",
        "autosave_exact", "hidden_by_hotkey", "ball_visible_when_hidden",
        "ball_noactivate", "ball_capture_valid", "ball_uses_brand_logo",
        "ball_no_legacy_yellow", "ball_no_legacy_square", "foreground_not_stolen_by_ball",
        "shown_by_hotkey", "ball_hidden_when_shown", "clean_exit",
        "shot_build_present",
    ]
    if checks.get("copy_all_plain_text") is not None:
        required += ["copy_all_plain_text", "copy_all_no_extra_format"]
    if checks.get("autostart_toggle_flips_state") is not None:
        required += ["autostart_toggle_flips_state", "autostart_toggle_restores",
                     "autostart_command_quoted", "autostart_command_matches_exe",
                     "autostart_state_restored"]
    if checks.get("geometry_first_launch") is not None:
        required += ["geometry_first_launch", "geometry_applied",
                     "geometry_config_written", "geometry_first_exit",
                     "geometry_second_launch", "geometry_restored_position",
                     "geometry_restored_size", "geometry_second_exit",
                     "geometry_config_restored"]
    ok = all(checks.get(k) is True for k in required)
    out = {"ok": ok, "exe": EXE, "checks": checks}
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
