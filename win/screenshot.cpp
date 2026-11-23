// GSticky Windows 区域截图 —— 圈选 + 文字/画框/箭头 + 写剪贴板。
//
// 关键顺序（P0 实测结论）：先隐藏自家浮层 → 抓取整个虚拟屏到内存位图 →
// 再显示覆盖层。分层窗口和普通 topmost 窗口都会被 BitBlt 抓进图里，
// 所以「先抓后显」是硬约束，不能反过来。
//
// 全程物理像素：虚拟屏原点可能为负，混合 DPI 多屏下不使用主程序的 S()/L()。
// 零第三方依赖：只用系统 DLL；GdiAlphaBlend 从已链接的 gdi32.dll 取
// （msimg32.dll 导出的是 AlphaBlend，导出名不同，取错会静默退化）。
// 压暗不可用时保持底图原样，绝不涂黑——否则圈选时整屏不可见。

#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NOMINMAX
#include "screenshot.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

const wchar_t* kOverlayClass = L"GStickyShotOverlay_v1";
const wchar_t* kEditorClass = L"GStickyShotEditor_v1";
const COLORREF kAccent = RGB(235, 48, 48);      // 标注红
const COLORREF kSelFrame = RGB(0, 205, 220);    // 选区青

enum Tool { kText = 0, kRect = 1, kArrow = 2 };

struct Annotation {
    int kind = kRect;
    POINT a = {}, b = {};      // 裁剪图内的原图像素坐标
    std::wstring text;
};

struct State {
    HINSTANCE hinst = nullptr;
    HWND owner = nullptr;
    HWND overlay = nullptr, editor = nullptr, textEdit = nullptr;
    WNDPROC oldEditProc = nullptr;
    bool active = false;
    bool selecting = true;
    bool dragging = false;
    bool previewOn = false;

    int vx = 0, vy = 0, vw = 0, vh = 0;   // 虚拟屏（物理像素）
    HDC fullDc = nullptr;
    HBITMAP fullBmp = nullptr;
    HDC overlayDc = nullptr;
    HBITMAP overlayBmp = nullptr;
    HDC editorDc = nullptr;
    HBITMAP editorBmp = nullptr;
    HDC blackDc = nullptr;
    HBITMAP blackBmp = nullptr;
    HFONT font = nullptr;

    RECT sel = {};                          // 虚拟屏绝对坐标
    POINT dragFrom = {}, dragTo = {}, textAt = {};
    int tool = kRect;
    int toolbarH = 34;
    double dispScale = 1.0;
    int dispW = 0, dispH = 0, cropW = 0, cropH = 0;

    std::vector<Annotation> anns;
    Annotation preview;
};
State g;

typedef BOOL (WINAPI* PFN_AlphaBlend)(HDC, int, int, int, int,
                                       HDC, int, int, int, int, BLENDFUNCTION);
PFN_AlphaBlend pAlphaBlend = nullptr;

int DpiPx(int px) {
    HDC s = GetDC(nullptr);
    int dpi = s ? GetDeviceCaps(s, LOGPIXELSX) : 96;
    if (s) ReleaseDC(nullptr, s);
    if (dpi < 96) dpi = 96;
    return MulDiv(px, dpi, 96);
}

void InitAlphaBlend() {
    if (pAlphaBlend) return;
    // gdi32.dll 已随 -lgdi32 链接且必然加载，导出名是 GdiAlphaBlend。
    // msimg32.dll 导出的是 AlphaBlend —— 早期版本从 msimg32 取 GdiAlphaBlend，
    // 实测必然返回 NULL，于是 DimRect 退化成纯黑、整屏不可见（2026-11-23 修复）。
    HMODULE m = GetModuleHandleW(L"gdi32.dll");
    if (m) pAlphaBlend = (PFN_AlphaBlend)(void*)GetProcAddress(m, "GdiAlphaBlend");
    if (!pAlphaBlend) {
        HMODULE ms = LoadLibraryW(L"msimg32.dll");
        if (ms) pAlphaBlend = (PFN_AlphaBlend)(void*)GetProcAddress(ms, "AlphaBlend");
    }
}

// ---- 绘制辅助 -------------------------------------------------------------
void DimRect(HDC dc, const RECT& r) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    // 压暗不可用时直接保留底图：早期退化分支会 FillRect(BLACK_BRUSH) 把整屏
    // 涂成纯黑，用户根本看不到要圈选的内容。选区亮框 + 顶部提示已足够引导。
    if (!pAlphaBlend || !g.blackDc) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 110;   // 约 43% 黑，选区外变暗但内容仍可见
    pAlphaBlend(dc, r.left, r.top, w, h, g.blackDc, 0, 0, 1, 1, bf);
}

void FrameRectEx(HDC dc, const RECT& r, int width, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBr);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawArrow(HDC dc, POINT a, POINT b, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, a.x, a.y, nullptr);
    LineTo(dc, b.x, b.y);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    double ang = atan2((double)(b.y - a.y), (double)(b.x - a.x));
    double len = 10.0 + width * 3.0;
    POINT pts[3];
    pts[0] = b;
    pts[1].x = (LONG)(b.x - len * cos(ang - 0.45));
    pts[1].y = (LONG)(b.y - len * sin(ang - 0.45));
    pts[2].x = (LONG)(b.x - len * cos(ang + 0.45));
    pts[2].y = (LONG)(b.y - len * sin(ang + 0.45));
    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldP2 = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, 3);
    SelectObject(dc, oldP2);
    SelectObject(dc, oldBr);
    DeleteObject(br);
}

void DrawOne(HDC dc, const Annotation& an, double sc) {
    POINT a = { (LONG)(an.a.x * sc), (LONG)(an.a.y * sc) };
    POINT b = { (LONG)(an.b.x * sc), (LONG)(an.b.y * sc) };
    int w = (int)(2 * sc + 0.5);
    if (w < 1) w = 1;
    if (an.kind == kRect) {
        RECT r = { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
        FrameRectEx(dc, r, w, kAccent);
    } else if (an.kind == kArrow) {
        DrawArrow(dc, a, b, kAccent, w);
    } else if (an.kind == kText && !an.text.empty()) {
        SetTextColor(dc, kAccent);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, a.x, a.y, an.text.c_str(), (int)an.text.size());
    }
}

void DrawAnnotations(HDC dc, double sc) {
    HGDIOBJ oldFont = g.font ? SelectObject(dc, g.font) : nullptr;
    for (const Annotation& an : g.anns) DrawOne(dc, an, sc);
    if (g.previewOn) DrawOne(dc, g.preview, sc);
    if (oldFont) SelectObject(dc, oldFont);
}

// ---- 覆盖层：冻结画面 + 选区外变暗 + 亮框 --------------------------------
void RenderOverlay() {
    if (!g.overlayDc) return;
    BitBlt(g.overlayDc, 0, 0, g.vw, g.vh, g.fullDc, 0, 0, SRCCOPY);
    RECT full = { 0, 0, g.vw, g.vh };
    if (g.dragging || g.sel.right > g.sel.left) {
        RECT s = g.sel;
        s.left -= g.vx; s.right -= g.vx; s.top -= g.vy; s.bottom -= g.vy;
        s.left = std::max(0, std::min(g.vw, (int)s.left));
        s.top = std::max(0, std::min(g.vh, (int)s.top));
        s.right = std::max(0, std::min(g.vw, (int)s.right));
        s.bottom = std::max(0, std::min(g.vh, (int)s.bottom));
        if (s.right > s.left && s.bottom > s.top) {
            DimRect(g.overlayDc, RECT{0, 0, g.vw, s.top});
            DimRect(g.overlayDc, RECT{0, s.bottom, g.vw, g.vh});
            DimRect(g.overlayDc, RECT{0, s.top, s.left, s.bottom});
            DimRect(g.overlayDc, RECT{s.right, s.top, g.vw, s.bottom});
            FrameRectEx(g.overlayDc, s, 2, kSelFrame);
        } else {
            DimRect(g.overlayDc, full);
        }
    } else {
        DimRect(g.overlayDc, full);
    }
    // 顶部提示
    HFONT old = g.font ? (HFONT)SelectObject(g.overlayDc, g.font) : nullptr;
    SetBkMode(g.overlayDc, TRANSPARENT);
    SetTextColor(g.overlayDc, RGB(255, 255, 255));
    RECT tip = { 12, 8, g.vw - 12, 8 + DpiPx(26) };
    DrawTextW(g.overlayDc, L"拖动鼠标圈选区域   ·   Esc 取消", -1, &tip,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (old) SelectObject(g.overlayDc, old);
}

void RequestOverlayPaint() {
    RenderOverlay();
    if (g.overlay) RedrawWindow(g.overlay, nullptr, nullptr,
                                RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

// ---- 编辑器：工具条 + 裁剪图 + 标注 ---------------------------------------
const wchar_t* kToolLabels[6] = {
    L"1 文字", L"2 画框", L"3 箭头", L"Z 撤销", L"Enter 完成", L"Esc 取消"
};

void RenderEditor() {
    if (!g.editorDc) return;
    HDC dc = g.editorDc;
    RECT bar = { 0, 0, g.dispW, g.toolbarH };
    FillRect(dc, &bar, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HFONT oldFont = g.font ? (HFONT)SelectObject(dc, g.font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    int bw = g.dispW / 6;
    for (int i = 0; i < 6; ++i) {
        RECT r = { i * bw, 0, (i + 1) * bw, g.toolbarH };
        if (i <= 2 && i == g.tool) {
            HBRUSH hb = CreateSolidBrush(RGB(220, 240, 245));
            FillRect(dc, &r, hb);
            DeleteObject(hb);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(205, 205, 205));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, r.right, 0, nullptr);
        LineTo(dc, r.right, g.toolbarH);
        SelectObject(dc, op);
        DeleteObject(pen);
        RECT t = r;
        t.left += 4; t.right -= 4;
        SetTextColor(dc, (i <= 2 && i == g.tool) ? RGB(0, 90, 110) : RGB(30, 30, 30));
        DrawTextW(dc, kToolLabels[i], -1, &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // 底图（原图像素 → 显示尺寸）
    StretchBlt(dc, 0, g.toolbarH, g.dispW, g.dispH,
               g.fullDc, g.sel.left - g.vx, g.sel.top - g.vy, g.cropW, g.cropH, SRCCOPY);
    // 标注用显示坐标绘制：临时平移 DC 原点，复用 DrawOne 的坐标换算
    // 这里 sc = dispScale，标注坐标本身就是原图坐标
    SetWindowOrgEx(dc, 0, -g.toolbarH, nullptr);
    DrawAnnotations(dc, g.dispScale);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    if (oldFont) SelectObject(dc, oldFont);
}

void RequestEditorPaint() {
    RenderEditor();
    if (g.editor) RedrawWindow(g.editor, nullptr, nullptr,
                               RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

// ---- 文字输入 -------------------------------------------------------------
void DestroyTextEdit(bool commit);

LRESULT CALLBACK TextEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { DestroyTextEdit(true); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyTextEdit(false); return 0; }
    return CallWindowProcW(g.oldEditProc, h, m, w, l);
}

void DestroyTextEdit(bool commit) {
    if (!g.textEdit) return;
    HWND h = g.textEdit;
    g.textEdit = nullptr;
    wchar_t buf[512] = {};
    if (commit) GetWindowTextW(h, buf, 512);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g.oldEditProc);
    DestroyWindow(h);
    if (commit && buf[0]) {
        Annotation an;
        an.kind = kText;
        an.a = g.textAt;
        an.text = buf;
        g.anns.push_back(an);
    }
    RequestEditorPaint();
}

void BeginTextEdit(int dispX, int dispY) {
    DestroyTextEdit(false);
    g.textAt.x = (LONG)(dispX / g.dispScale);
    g.textAt.y = (LONG)(dispY / g.dispScale);
    int w = DpiPx(220), h = DpiPx(26);
    g.textEdit = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 dispX, dispY + g.toolbarH, w, h,
                                 g.editor, (HMENU)901, g.hinst, nullptr);
    if (!g.textEdit) return;
    if (g.font) SendMessageW(g.textEdit, WM_SETFONT, (WPARAM)g.font, TRUE);
    g.oldEditProc = (WNDPROC)SetWindowLongPtrW(g.textEdit, GWLP_WNDPROC,
                                                (LONG_PTR)TextEditProc);
    SetFocus(g.textEdit);
}

// ---- 输出：合成裁剪图 + 标注 → CF_DIB --------------------------------------
bool ComposeAndCopyToClipboard() {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC outDc = CreateCompatibleDC(screen);
    HBITMAP outBmp = CreateCompatibleBitmap(screen, g.cropW, g.cropH);
    if (!outDc || !outBmp) {
        if (outBmp) DeleteObject(outBmp);
        if (outDc) DeleteDC(outDc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(outDc, outBmp);
    BitBlt(outDc, 0, 0, g.cropW, g.cropH, g.fullDc,
           g.sel.left - g.vx, g.sel.top - g.vy, SRCCOPY);
    HGDIOBJ oldFont = g.font ? SelectObject(outDc, g.font) : nullptr;
    SetBkMode(outDc, TRANSPARENT);
    for (const Annotation& an : g.anns) DrawOne(outDc, an, 1.0);
    if (oldFont) SelectObject(outDc, oldFont);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g.cropW;
    bi.bmiHeader.biHeight = -g.cropH;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels((size_t)g.cropW * g.cropH * 4);
    int lines = GetDIBits(outDc, outBmp, 0, g.cropH, pixels.data(),
                          &bi, DIB_RGB_COLORS);
    SelectObject(outDc, oldBmp);
    DeleteObject(outBmp);
    DeleteDC(outDc);
    ReleaseDC(nullptr, screen);
    if (lines != g.cropH) return false;

    // 32bpp BI_RGB 的第 4 字节名义上保留不用，但 GetDIBits 从 DDB 取回时通常为 0；
    // WPF / 部分浏览器会把它当 alpha 解释，整张图变全透明。统一填不透明。
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;

    size_t blob = sizeof(BITMAPINFOHEADER) + pixels.size();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, blob);
    if (!h) return false;
    BYTE* p = (BYTE*)GlobalLock(h);
    if (!p) { GlobalFree(h); return false; }
    memcpy(p, &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    memcpy(p + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
    GlobalUnlock(h);

    bool ok = false;
    for (int i = 0; i < 3 && !ok; ++i) {   // 剪贴板可能被别的进程短暂占用
        if (OpenClipboard(g.owner)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, h) != nullptr;
            CloseClipboard();
            if (ok) h = nullptr;           // 所有权已移交系统
        } else {
            Sleep(60);
        }
    }
    if (h) GlobalFree(h);
    return ok;
}

// ---- 会话生命周期 ---------------------------------------------------------
void ReleaseGdi() {
    if (g.textEdit) { DestroyWindow(g.textEdit); g.textEdit = nullptr; }
    if (g.editor) { DestroyWindow(g.editor); g.editor = nullptr; }
    if (g.overlay) { DestroyWindow(g.overlay); g.overlay = nullptr; }
    // MakeBuffer 创建的位图始终被对应 DC 选中：必须先销毁 DC，再删位图。
    // 反过来 DeleteObject 会失败，每次会话泄漏 4 个 GDI 位图。
    if (g.editorDc) DeleteDC(g.editorDc);
    if (g.editorBmp) DeleteObject(g.editorBmp);
    if (g.overlayDc) DeleteDC(g.overlayDc);
    if (g.overlayBmp) DeleteObject(g.overlayBmp);
    if (g.fullDc) DeleteDC(g.fullDc);
    if (g.fullBmp) DeleteObject(g.fullBmp);
    if (g.blackDc) DeleteDC(g.blackDc);
    if (g.blackBmp) DeleteObject(g.blackBmp);
    if (g.font) DeleteObject(g.font);
    g = State();
}

void EndSession(bool copied) {
    if (!g.active) return;
    HWND owner = g.owner;
    g.active = false;
    ReleaseGdi();
    if (owner && IsWindow(owner))
        PostMessageW(owner, gsticky_shot::kMsgShotDone, copied ? 1 : 0, 0);
}

bool MakeBuffer(HDC& dc, HBITMAP& bmp, int w, int h, HDC screen) {
    dc = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, w, h);
    if (!dc || !bmp) return false;
    SelectObject(dc, bmp);
    return true;
}

bool CaptureScreen(HDC screen) {
    g.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g.vw <= 0 || g.vh <= 0) return false;
    if (!MakeBuffer(g.fullDc, g.fullBmp, g.vw, g.vh, screen)) return false;
    return BitBlt(g.fullDc, 0, 0, g.vw, g.vh, screen, g.vx, g.vy,
                  SRCCOPY | CAPTUREBLT) != FALSE;
}

void ShowEditor();

// ---- 窗口过程 -------------------------------------------------------------
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        if (g.overlayDc)
            BitBlt(dc, 0, 0, g.vw, g.vh, g.overlayDc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        p.x += g.vx; p.y += g.vy;
        g.dragFrom = g.dragTo = p;
        g.dragging = true;
        g.sel = { p.x, p.y, p.x, p.y };
        SetCapture(hwnd);
        RequestOverlayPaint();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g.dragging) return 0;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        p.x += g.vx; p.y += g.vy;
        g.dragTo = p;
        g.sel.left = std::min(g.dragFrom.x, p.x);
        g.sel.top = std::min(g.dragFrom.y, p.y);
        g.sel.right = std::max(g.dragFrom.x, p.x);
        g.sel.bottom = std::max(g.dragFrom.y, p.y);
        RequestOverlayPaint();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!g.dragging) return 0;
        g.dragging = false;
        ReleaseCapture();
        if (g.sel.right - g.sel.left < 8 || g.sel.bottom - g.sel.top < 8) {
            g.sel = {};
            RequestOverlayPaint();       // 选区过小：留在覆盖层，允许重选
            return 0;
        }
        ShowEditor();
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { EndSession(false); return 0; }
        return 0;
    case WM_CLOSE:
        // Alt+F4 / SC_CLOSE 必须走 EndSession：DefWindowProc 会直接销毁窗口，
        // active 永久置位，便签/悬浮球不还原、显隐热键被挡死。
        EndSession(false);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        if (g.editorDc)
            BitBlt(dc, 0, 0, g.dispW, g.toolbarH + g.dispH, g.editorDc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < g.toolbarH) {
            DestroyTextEdit(true);
            int bw = g.dispW / 6;
            int idx = bw > 0 ? std::min(5, x / bw) : 5;
            if (idx <= 2) { g.tool = idx; RequestEditorPaint(); }
            else if (idx == 3) {
                if (!g.anns.empty()) g.anns.pop_back();
                RequestEditorPaint();
            } else if (idx == 4) {
                EndSession(ComposeAndCopyToClipboard());
            } else {
                EndSession(false);
            }
            return 0;
        }
        int dx = x, dy = y - g.toolbarH;
        if (dx < 0 || dy < 0 || dx > g.dispW || dy > g.dispH) return 0;
        if (g.tool == kText) {
            BeginTextEdit(dx, dy);
            return 0;
        }
        DestroyTextEdit(true);
        g.dragFrom = g.dragTo = {
            (LONG)(dx / g.dispScale), (LONG)(dy / g.dispScale) };
        g.dragging = true;
        g.previewOn = true;
        g.preview = Annotation();
        g.preview.kind = g.tool;
        g.preview.a = g.dragFrom;
        g.preview.b = g.dragTo;
        SetCapture(hwnd);
        RequestEditorPaint();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g.dragging) return 0;
        int dx = GET_X_LPARAM(lp), dy = GET_Y_LPARAM(lp) - g.toolbarH;
        g.preview.b = { (LONG)(dx / g.dispScale), (LONG)(dy / g.dispScale) };
        RequestEditorPaint();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!g.dragging) return 0;
        g.dragging = false;
        g.previewOn = false;
        ReleaseCapture();
        Annotation an = g.preview;
        int ddx = (int)(an.b.x - an.a.x), ddy = (int)(an.b.y - an.a.y);
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;
        if (ddx >= 4 || ddy >= 4)
            g.anns.push_back(an);
        RequestEditorPaint();
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE) { EndSession(false); return 0; }
        if (wp == VK_RETURN) { DestroyTextEdit(true); EndSession(ComposeAndCopyToClipboard()); return 0; }
        if (wp == '1') { g.tool = kText; RequestEditorPaint(); return 0; }
        if (wp == '2') { g.tool = kRect; RequestEditorPaint(); return 0; }
        if (wp == '3') { g.tool = kArrow; RequestEditorPaint(); return 0; }
        if (wp == 'Z') {           // 单按 Z 或 Ctrl+Z 都撤销一步（与 README 一致）
            if (!g.anns.empty()) g.anns.pop_back();
            RequestEditorPaint();
            return 0;
        }
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)lp == g.textEdit && HIWORD(wp) == EN_KILLFOCUS) {
            DestroyTextEdit(true);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        EndSession(false);   // 同 OverlayProc：Alt+F4 取消会话而非裸销毁
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterWindows(HINSTANCE hinst) {
    WNDCLASSEXW a = {};
    a.cbSize = sizeof(a);
    a.hInstance = hinst;
    a.lpfnWndProc = OverlayProc;
    a.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    a.hbrBackground = nullptr;   // 内容全部由内存 DC 自绘，禁止系统擦背景刷黑
    a.lpszClassName = kOverlayClass;
    RegisterClassExW(&a);

    WNDCLASSEXW b = {};
    b.cbSize = sizeof(b);
    b.hInstance = hinst;
    b.lpfnWndProc = EditorProc;
    b.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    b.hbrBackground = nullptr;   // 同上：首帧不能刷成白色或黑色
    b.lpszClassName = kEditorClass;
    RegisterClassExW(&b);
}

void ShowEditor() {
    g.selecting = false;
    g.dragging = false;
    g.previewOn = false;
    g.cropW = g.sel.right - g.sel.left;
    g.cropH = g.sel.bottom - g.sel.top;

    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int maxW = (wa.right - wa.left) * 85 / 100;
    int maxH = (wa.bottom - wa.top) * 80 / 100 - DpiPx(40);
    if (maxW < 200) maxW = 200;
    if (maxH < 150) maxH = 150;
    g.dispScale = std::min(1.0, std::min((double)maxW / g.cropW, (double)maxH / g.cropH));
    g.dispW = std::max(1, (int)(g.cropW * g.dispScale));
    g.dispH = std::max(1, (int)(g.cropH * g.dispScale));
    g.toolbarH = DpiPx(34);

    HDC screen = GetDC(nullptr);
    if (!MakeBuffer(g.editorDc, g.editorBmp, g.dispW, g.toolbarH + g.dispH, screen)) {
        if (screen) ReleaseDC(nullptr, screen);
        EndSession(false);
        return;
    }
    ReleaseDC(nullptr, screen);

    int w = g.dispW, h = g.toolbarH + g.dispH;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    g.editor = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kEditorClass,
                               L"GSticky 截图编辑", WS_POPUP,
                               x, y, w, h, nullptr, nullptr, g.hinst, nullptr);
    if (!g.editor) { EndSession(false); return; }
    RenderEditor();               // 同上：避免首帧画出未初始化的内存位图
    ShowWindow(g.overlay, SW_HIDE);
    ShowWindow(g.editor, SW_SHOW);
    SetForegroundWindow(g.editor);
    SetFocus(g.editor);
    RequestEditorPaint();
}

}  // namespace

namespace gsticky_shot {

bool IsActive() { return g.active; }

void Cancel() { EndSession(false); }

bool Start(HINSTANCE hinst, HWND owner) {
    if (g.active) return false;
    // 启动阶段就置位：Sleep / 4K 抓屏期间再点菜单不会重复进入。
    // 所有失败路径都会走 ReleaseGdi()（其中 g = State() 把 active 复位）。
    g.active = true;
    InitAlphaBlend();
    RegisterWindows(hinst);
    // 调用方已隐藏自家浮层、托盘菜单也已销毁；等 DWM 合成一帧再抓，
    // 否则刚隐藏的窗口可能仍留在桌面 DC 的像素里（P0 实测分层窗口会入图）。
    Sleep(80);
    if (!g.active) return false;   // 期间被 Cancel（例如程序退出）

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    g.hinst = hinst;
    g.owner = owner;
    g.selecting = true;
    g.tool = kRect;
    g.anns.clear();
    g.font = CreateFontW(-DpiPx(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
    if (!CaptureScreen(screen)) {
        ReleaseDC(nullptr, screen);
        ReleaseGdi();
        return false;
    }
    if (!MakeBuffer(g.overlayDc, g.overlayBmp, g.vw, g.vh, screen)) {
        ReleaseDC(nullptr, screen);
        ReleaseGdi();
        return false;
    }
    // 1x1 纯黑位图：AlphaBlend 用它给选区外区域叠半透明黑
    DWORD blackPixel = 0;
    g.blackBmp = CreateBitmap(1, 1, 1, 32, &blackPixel);
    g.blackDc = CreateCompatibleDC(screen);
    if (g.blackBmp && g.blackDc) SelectObject(g.blackDc, g.blackBmp);
    ReleaseDC(nullptr, screen);

    g.overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"",
                                WS_POPUP, g.vx, g.vy, g.vw, g.vh,
                                nullptr, nullptr, hinst, nullptr);
    if (!g.overlay) { ReleaseGdi(); return false; }
    // 先填好内存位图再显示：CreateCompatibleBitmap 内容未定义（通常是黑），
    // 否则首次 WM_PAINT 会把未初始化的位图画出来，形成一帧全黑闪屏。
    RenderOverlay();
    // active 已在 Start 开头置位，覆盖层的 WM_* 回调此时即可生效
    ShowWindow(g.overlay, SW_SHOW);
    SetForegroundWindow(g.overlay);
    SetFocus(g.overlay);
    RequestOverlayPaint();
    return true;
}

// 自检：纯内存复现覆盖层压暗路径，确保不会退化成全黑（黑屏 bug 的回归项）。
// 不建窗口、不抓屏、不碰剪贴板。
bool SelfTestDim() {
    InitAlphaBlend();
    if (!pAlphaBlend) return false;   // 函数没取到 = 曾经的黑屏根因，直接判失败
    const int w = 8, h = 8;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0xFF, (size_t)w * h * 4);   // 纯白不透明

    DWORD zero = 0;
    HBITMAP black = CreateBitmap(1, 1, 1, 32, &zero);
    HDC blackDc = CreateCompatibleDC(screen);
    HGDIOBJ oldBlack = nullptr;
    if (black && blackDc) oldBlack = SelectObject(blackDc, black);

    bool ok = false;
    if (black && blackDc) {
        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 110;
        ok = pAlphaBlend(dc, 0, 0, w, h, blackDc, 0, 0, 1, 1, bf) != FALSE;
        const BYTE* p = (const BYTE*)bits;
        // 期望约 145：既不能是 0（全黑），也不能是 255（完全没压暗）
        ok = ok && p[0] > 40 && p[0] < 230 && p[1] == p[0] && p[2] == p[0];
    }

    if (blackDc) {
        if (oldBlack) SelectObject(blackDc, oldBlack);
        DeleteDC(blackDc);
    }
    if (black) DeleteObject(black);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool SelfTestDib() {
    const int w = 8, h = 8;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!dc || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, white);
    DeleteObject(white);
    HBRUSH red = CreateSolidBrush(RGB(0x28, 0x1E, 0xC8));   // BGRA = 200,30,40
    RECT r = { 1, 1, 6, 6 };
    FillRect(dc, &r, red);
    DeleteObject(red);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels((size_t)w * h * 4);
    int lines = GetDIBits(dc, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);

    HBITMAP back = CreateDIBitmap(screen, &bi.bmiHeader, CBM_INIT, pixels.data(),
                                  &bi, DIB_RGB_COLORS);
    bool roundtrip = false;
    if (back) {
        HDC dc2 = CreateCompatibleDC(screen);
        HGDIOBJ old2 = SelectObject(dc2, back);
        BITMAPINFO bi2 = bi;
        std::vector<BYTE> again((size_t)w * h * 4);
        int lines2 = GetDIBits(dc2, back, 0, h, again.data(), &bi2, DIB_RGB_COLORS);
        bool same = true;
        for (int i = 0; i < w * h * 4; i += 4) {
            if (pixels[i] != again[i] || pixels[i + 1] != again[i + 1] ||
                pixels[i + 2] != again[i + 2]) { same = false; break; }
        }
        size_t idx = ((size_t)2 * w + 2) * 4;
        bool painted = lines == h && lines2 == h &&
                       pixels[idx] == 0xC8 && pixels[idx + 1] == 0x1E &&
                       pixels[idx + 2] == 0x28;
        roundtrip = same && painted && bi.bmiHeader.biSize == sizeof(BITMAPINFOHEADER);
        SelectObject(dc2, old2);
        DeleteObject(back);
        DeleteDC(dc2);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return roundtrip;
}

}  // namespace gsticky_shot
