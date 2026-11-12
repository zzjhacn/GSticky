// GSticky for Windows — 单文件 C++/Win32 实现，零第三方依赖、静态链接单 exe。
//
// 设计口径（见 win/VERIFICATION.md）：只覆盖「普通窗口最大化、任务栏可见」的日常场景。
// 不做周期性重夺层级、不做跨虚拟桌面、不做全屏状态检测。
//
// 与 macOS 版（macos/main.swift）的规格对齐：
//   - 320x260 置顶便签，停靠工作区右上角；红叉 = 隐藏（不退出）
//   - 纯文本编辑（Win32 Edit 天然纯文本），复制全部只写 CF_UNICODETEXT
//   - 0.5s debounce 自动保存，UTF-8 明文，LF 换行与 macOS 共用同一份文件
//   - Ctrl+Alt+G 全局热键；托盘常驻（显示/隐藏、复制全部、开机自启开关、退出）
//   - 44x44 圆形悬浮球（系统色圆底 + 品牌云标），3px 阈值区分点击/拖动，
//     按显示器记忆落点，与便签互斥
//
// 构建：win\build.bat   （MinGW-w64 / GCC，windres 嵌图标 + g++ -mwindows -static）
// 图标：win/GSticky.ico（win/make_icon.py 从项目根 icon.png 生成）经 win/GSticky.rc 嵌入；
//       exe 文件图标、窗口/任务栏、托盘、悬浮球中心图案共用这一份资源，与 macOS 版同源。
// 自检：GSticky.exe --selftest  → %APPDATA%\GSticky\selftest.json
// 数据：%APPDATA%\GSticky\note.txt（UTF-8 / LF，无 BOM）
// 自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的 GSticky 值，
//       由托盘「开机自启」开关管理，默认关闭；无额外配置文件、无需管理员权限。

#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <cwchar>
#include <string>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
// 托盘回调的事件码（宽松兼容 V1 与 VERSION_4 两种语义）
static const UINT kNinSelect = 0x0400;
static const UINT kNinKeySelect = 0x0401;

// ---- 可调常量 -------------------------------------------------------------
static const bool kActivateOnShow = true;  // 唤出时取得焦点，可直接打字（对齐 macOS makeKeyAndOrderFront）
static const int  kBaseNoteW = 320, kBaseNoteH = 260;
static const int  kBaseBallSize = 44;
static const int  kNoteMargin = 20, kBallMargin = 16;
static const UINT kSaveDebounceMs = 500;
static const BYTE kBallAlpha = 225;

static const wchar_t* kNoteClass = L"GStickyNoteWindow_v1";
static const wchar_t* kBallClass = L"GStickyBallWindow_v1";
static const wchar_t* kMutexName = L"Local\\GSticky_SingleInstance_v1";

static const UINT kHotkeyId = 1;
static const UINT kTimerSave = 2;
static const UINT kTimerSelftest = 3;
static const UINT kTimerFlash = 4;
static const UINT kMsgTray = WM_APP + 1;
static const UINT kMsgActivate = WM_APP + 2;
static const UINT kIdShow = 4001, kIdCopyAll = 4002, kIdQuit = 4003;
static const UINT kIdAutoStart = 4004;
static const int  kIconId = 101;  // 必须与 win/GSticky.rc 的 IDI_GSTICKY 一致

// 开机自启：HKCU Run 项是唯一状态源（不另写配置文件），无需管理员权限。
// 值由托盘菜单开关管理，默认关闭；exe 路径变化时启动时幂等自愈。
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"GSticky";
// 自检专用键：绝不读写用户真实的 Run 项，测完即删。
static const wchar_t* kSelftestKey = L"Software\\GStickySelftest";
static const wchar_t* kSelftestValue = L"Probe";

static const COLORREF kNoteBg = RGB(255, 248, 220);   // 淡黄便签
static const COLORREF kInk = RGB(38, 34, 26);
static const int  kBaseBallIcon = 24;  // 悬浮球内品牌标尺寸，与 macOS BallView 的 s=24 对齐

// ---- 全局状态 -------------------------------------------------------------
static HINSTANCE g_hinst = nullptr;
static HWND g_note = nullptr, g_edit = nullptr, g_ball = nullptr;
static HFONT g_font = nullptr;
static HBRUSH g_editBrush = nullptr;
// 三个图标句柄都取自同一份 exe 资源：窗口/任务栏、托盘、悬浮球中心
static HICON g_iconBig = nullptr, g_iconSmall = nullptr, g_iconBall = nullptr;
static bool g_iconsFromResource = false;  // false 时使用系统共享图标，不能 DestroyIcon
static NOTIFYICONDATAW g_nid = {};
static bool g_trayOk = false, g_hotkeyOk = false;
static bool g_noteVisible = true, g_loading = false, g_selftest = false;
static UINT g_dpi = 96;
static double g_scale = 1.0;

static std::wstring g_dataDir, g_notePath, g_configPath;
static std::wstring g_selftestNotePath, g_selftestOutPath;

static bool g_ballDragging = false, g_ballMoved = false;
static POINT g_ballDragStart = {}, g_ballOriginStart = {};

static LRESULT CALLBACK NoteProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK BallProc(HWND, UINT, WPARAM, LPARAM);

// ---- 小工具 ---------------------------------------------------------------
static int S(int px) { return (int)(px * g_scale + 0.5); }  // 逻辑像素 → 物理像素

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 与 macOS 共用 note.txt：保存时统一 CRLF/CR → LF，载入时 LF → CRLF（Edit 要求）
static std::string NormalizeLf(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

static std::wstring ToCrLf(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\r') {
            if (i + 1 < in.size() && in[i + 1] == L'\n') ++i;
            out += L"\r\n";
        } else if (in[i] == L'\n') {
            out += L"\r\n";
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

static bool WriteRawFile(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok && written == bytes.size();
}

static std::string ReadRawFile(const std::wstring& path, bool& ok) {
    ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return std::string(); }
    std::string s((size_t)size, '\0');
    DWORD read = 0;
    if (size) ReadFile(h, s.data(), size, &read, nullptr);
    CloseHandle(h);
    s.resize(read);
    if (s.size() >= 3 && (BYTE)s[0] == 0xEF && (BYTE)s[1] == 0xBB && (BYTE)s[2] == 0xBF)
        s.erase(0, 3);  // 容忍带 BOM 的外部文件
    ok = true;
    return s;
}

// DPI：优先 per-monitor v2（Win10 1703+），动态取地址以免依赖 SDK 版本
typedef BOOL (WINAPI* PFN_SetDpiAwarenessContext)(void*);
typedef UINT (WINAPI* PFN_GetDpiForSystem)(void);

static void InitDpi() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        void* raw = reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext"));
        auto pSet = reinterpret_cast<PFN_SetDpiAwarenessContext>(raw);
        if (pSet) pSet((void*)(INT_PTR)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();  // 兜底（上面的调用成功后此调用会失败，忽略即可）
    if (u) {
        void* raw = reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForSystem"));
        auto pSys = reinterpret_cast<PFN_GetDpiForSystem>(raw);
        if (pSys) {
            UINT d = pSys();
            if (d) g_dpi = d;
        }
    }
    g_scale = (double)g_dpi / 96.0;
}

static void InitPaths() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)) || !appdata[0])
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (!appdata[0]) GetCurrentDirectoryW(MAX_PATH, appdata);
    g_dataDir = std::wstring(appdata) + L"\\GSticky";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);  // 已存在时返回 FALSE，忽略
    g_notePath = g_dataDir + L"\\note.txt";
    g_configPath = g_dataDir + L"\\config.ini";
    g_selftestNotePath = g_dataDir + L"\\selftest_note.txt";
    g_selftestOutPath = g_dataDir + L"\\selftest.json";
}

// ---- 图标 ---------------------------------------------------------------------
// 唯一图标源：win/GSticky.ico 由 win/make_icon.py 从项目根 icon.png 生成，
// 再经 win/GSticky.rc + windres 嵌入 exe。exe 文件图标、窗口/任务栏图标、
// 托盘图标、悬浮球中心图案全部取自同一份资源，与 macOS 版 icon.png 同源。
static HICON LoadIconAt(int px) {
    return (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                             px, px, LR_DEFAULTCOLOR);
}

// 三个句柄有可能指向同一个共享图标（兜底路径），去重后再销毁
static void DestroyIconTrio(HICON a, HICON b, HICON c) {
    if (a && a != b && a != c) DestroyIcon(a);
    if (b && b != c) DestroyIcon(b);
    if (c) DestroyIcon(c);
}

static bool LoadAppIcons() {
    g_iconBig = LoadIconAt(S(32));                  // 窗口 / 任务栏 / Alt+Tab
    g_iconSmall = LoadIconAt(S(16));                // 托盘 / 标题栏小图标
    g_iconBall = LoadIconAt(S(kBaseBallIcon));      // 悬浮球中心品牌云标
    g_iconsFromResource = (g_iconBig && g_iconSmall && g_iconBall);
    if (!g_iconsFromResource) {  // 资源缺失时兜底为系统共享图标（不可 DestroyIcon）
        DestroyIconTrio(g_iconBig, g_iconSmall, g_iconBall);
        g_iconBig = LoadIconW(nullptr, IDI_APPLICATION);
        g_iconSmall = g_iconBig;
        g_iconBall = g_iconBig;
    }
    return g_iconBig != nullptr;
}

static void ApplyIcons() {
    SendMessageW(g_note, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
    SendMessageW(g_note, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
    if (g_trayOk) {
        g_nid.uFlags = NIF_ICON;
        g_nid.hIcon = g_iconSmall;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

// DPI 变化后按新缩放重取全部图标，避免高分屏下托盘/标题栏/悬浮球图标被拉伸模糊
static void ReloadIconsOnDpi() {
    if (!g_iconsFromResource) return;
    HICON big = LoadIconAt(S(32)), small = LoadIconAt(S(16));
    HICON ball = LoadIconAt(S(kBaseBallIcon));
    if (!big || !small || !ball) {
        DestroyIconTrio(big, small, ball);
        return;
    }
    HICON oldBig = g_iconBig, oldSmall = g_iconSmall, oldBall = g_iconBall;
    g_iconBig = big;
    g_iconSmall = small;
    g_iconBall = ball;
    ApplyIcons();  // 先让窗口/托盘改用新句柄，再销毁旧句柄
    DestroyIconTrio(oldBig, oldSmall, oldBall);
}

static void RebuildFont() {
    HFONT old = g_font;
    g_font = CreateFontW(-MulDiv(13, g_dpi, 72), 0, 0, 0, FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (g_edit) SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (old) DeleteObject(old);
}

// ---- 存储 / 剪贴板 --------------------------------------------------------
static std::wstring GetEditText() {
    if (!g_edit) return std::wstring();
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) return std::wstring();
    std::wstring s((size_t)len, L'\0');
    GetWindowTextW(g_edit, s.data(), len + 1);
    return s;
}

static bool SaveNoteTo(const std::wstring& path, const std::wstring& text) {
    std::string bytes = NormalizeLf(ToUtf8(text));  // UTF-8 无 BOM，LF 换行
    std::wstring tmp = path + L".tmp";
    if (!WriteRawFile(tmp, bytes)) return false;
    BOOL ok = MoveFileExW(tmp.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok != FALSE;
}

static bool SaveNow() { return SaveNoteTo(g_notePath, GetEditText()); }

static void LoadNote() {
    bool ok = false;
    std::string bytes = ReadRawFile(g_notePath, ok);
    if (!ok || bytes.empty()) return;
    g_loading = true;
    SetWindowTextW(g_edit, ToCrLf(FromUtf8(bytes)).c_str());
    g_loading = false;
}

// 只写 CF_UNICODETEXT：源内容逐字复制，不夹带 RTF / 样式（治本点）
static bool CopyAll() {
    std::wstring s = GetEditText();
    if (!OpenClipboard(g_note)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) {
            memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// ---- 开机自启 -------------------------------------------------------------
// 用 HKCU\...\Run 下的一个 REG_SZ 值，格式为 "exe 绝对路径"（带引号防空格）。
// 它就是开关状态的唯一来源：菜单每次现读现显示，不另存 config，避免双状态不一致。
// 不用 HKLM / 任务计划 / 服务 —— 那些要管理员权限或额外组件，与单 exe 分发相悖。
static std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);  // 超长路径：扩容重试
    }
}

static std::wstring RunCommand(const std::wstring& exe) {
    return L"\"" + exe + L"\"";
}

static bool ReadRegString(const wchar_t* key, const wchar_t* name, std::wstring& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, bytes = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type, nullptr, &bytes);
    bool ok = false;
    if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
        bytes >= sizeof(wchar_t)) {
        std::wstring v(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD got = bytes;
        if (RegQueryValueExW(h, name, nullptr, &type, (LPBYTE)v.data(), &got) == ERROR_SUCCESS) {
            size_t z = 0;
            while (z < v.size() && v[z] != L'\0') ++z;  // 去掉结尾 NUL
            v.resize(z);
            out = v;
            ok = true;
        }
    }
    RegCloseKey(h);
    return ok;
}

static bool WriteRegString(const wchar_t* key, const wchar_t* name, const std::wstring& data) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)data.c_str(),
                            (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static bool DeleteRegValue(const wchar_t* key, const wchar_t* name) {
    HKEY h = nullptr;
    // 键本身不存在即视为「已删除」
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return true;
    LONG r = RegDeleteValueW(h, name);
    RegCloseKey(h);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

static bool AutoStartEnabled() {
    std::wstring cur;
    return ReadRegString(kRunKey, kRunValue, cur);
}

static bool SetAutoStart(bool enable) {
    if (!enable) return DeleteRegValue(kRunKey, kRunValue);
    std::wstring exe = ExePath();
    if (exe.empty()) return false;
    return WriteRegString(kRunKey, kRunValue, RunCommand(exe));
}

// exe 被移动 / 改名后 Run 项会指向旧路径，开机自启随之失效。
// 只在「已开启且路径确实变了」时刷新一次；未开启时完全不碰注册表。
// 非本程序写出的格式（不以引号开头）不擅自覆盖，尊重用户自定义命令行。
static void RefreshAutoStartPath() {
    std::wstring cur;
    if (!ReadRegString(kRunKey, kRunValue, cur)) return;
    if (cur.empty() || cur[0] != L'"') return;
    std::wstring exe = ExePath();
    if (exe.empty()) return;
    std::wstring want = RunCommand(exe);
    if (cur != want) WriteRegString(kRunKey, kRunValue, want);
}

// ---- 屏幕 / 定位 ----------------------------------------------------------
static HMONITOR ActiveMonitor() {
    HWND fg = GetForegroundWindow();
    return MonitorFromWindow(fg ? fg : g_note, MONITOR_DEFAULTTOPRIMARY);
}

static bool MonitorInfoFor(HMONITOR hm, MONITORINFOEXW& mi) {
    mi = {};
    mi.cbSize = sizeof(MONITORINFOEXW);
    return hm && GetMonitorInfoW(hm, (MONITORINFO*)&mi) != FALSE;
}

static RECT WorkAreaFor(HMONITOR hm) {
    MONITORINFOEXW mi;
    if (MonitorInfoFor(hm, mi)) return mi.rcWork;
    RECT r = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// 显示器配置变化后把便签拉回工作区右上角（保留用户调过的尺寸）
// 关键：坐标基准是工作区（任务栏可见），不是屏幕矩形
static void PositionNote() {
    RECT wa = WorkAreaFor(ActiveMonitor());
    RECT r = {};
    GetWindowRect(g_note, &r);
    int w = r.right - r.left, h = r.bottom - r.top, m = S(kNoteMargin);
    if (w <= 0) w = S(kBaseNoteW);
    if (h <= 0) h = S(kBaseNoteH);
    SetWindowPos(g_note, HWND_TOPMOST, wa.right - w - m, wa.top + m, w, h,
                 SWP_NOACTIVATE);
}

static bool ParseXY(const wchar_t* s, int& x, int& y) {
    const wchar_t* comma = wcschr(s, L',');
    if (!comma) return false;
    x = (int)wcstol(s, nullptr, 10);
    y = (int)wcstol(comma + 1, nullptr, 10);
    return true;
}

static void PositionBall(bool useSaved) {
    HMONITOR hm = ActiveMonitor();
    MONITORINFOEXW mi;
    if (!MonitorInfoFor(hm, mi)) return;
    RECT wa = mi.rcWork;
    int sz = S(kBaseBallSize), m = S(kBallMargin);
    int x = wa.right - sz - m, y = wa.top + m;
    if (useSaved) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"ball", mi.szDevice, L"", buf, 64, g_configPath.c_str());
        int sx = 0, sy = 0;
        if (ParseXY(buf, sx, sy)) {
            // 落点必须仍在某个显示器范围内，避免拔屏后球跑到屏幕外
            RECT pr = { sx, sy, sx + sz, sy + sz };
            if (MonitorFromRect(&pr, MONITOR_DEFAULTTONULL)) { x = sx; y = sy; }
        }
    }
    SetWindowPos(g_ball, HWND_TOPMOST, x, y, sz, sz, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowRgn(g_ball, CreateEllipticRgn(0, 0, sz + 1, sz + 1), TRUE);
}

static void PersistBallPos() {
    MONITORINFOEXW mi;
    if (!MonitorInfoFor(MonitorFromWindow(g_ball, MONITOR_DEFAULTTONEAREST), mi)) return;
    RECT r = {};
    GetWindowRect(g_ball, &r);
    wchar_t buf[64];
    wsprintfW(buf, L"%d,%d", r.left, r.top);
    WritePrivateProfileStringW(L"ball", mi.szDevice, buf, g_configPath.c_str());
}

// ---- 托盘 / 显隐 ----------------------------------------------------------
static void InitTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_note;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kMsgTray;
    g_nid.hIcon = g_iconSmall;
    lstrcpynW(g_nid.szTip, L"GSticky · 纯文本浮动便签", 128);
    g_trayOk = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void FlashTrayTip(const wchar_t* text) {
    if (!g_trayOk) return;
    lstrcpynW(g_nid.szTip, text, 128);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    SetTimer(g_note, kTimerFlash, 1000, nullptr);
}

static void RestoreTrayTip() {
    if (!g_trayOk) return;
    lstrcpynW(g_nid.szTip, L"GSticky · 纯文本浮动便签", 128);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowTrayMenu() {
    POINT pt = {};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_noteVisible ? MF_CHECKED : 0), kIdShow,
                L"显示 / 隐藏便签  (Ctrl+Alt+G)");
    AppendMenuW(menu, MF_STRING, kIdCopyAll, L"复制全部（纯文本）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (AutoStartEnabled() ? MF_CHECKED : 0), kIdAutoStart,
                L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"退出 GSticky");
    SetForegroundWindow(g_note);  // 否则菜单不会自动消失
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, g_note, nullptr);
    PostMessageW(g_note, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void ForceForeground(HWND h) {
    if (SetForegroundWindow(h)) return;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD fgTid = GetWindowThreadProcessId(fg, nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) {
        AttachThreadInput(myTid, fgTid, TRUE);
        SetForegroundWindow(h);
        BringWindowToTop(h);
        AttachThreadInput(myTid, fgTid, FALSE);
    }
}

static void SetNoteVisible(bool show, bool activate) {
    g_noteVisible = show;
    if (show) {
        ShowWindow(g_ball, SW_HIDE);
        ShowWindow(g_note, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (g_note) SetWindowPos(g_note, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (activate && !g_selftest) {
            ForceForeground(g_note);
            SetFocus(g_edit);
            int len = GetWindowTextLengthW(g_edit);
            SendMessageW(g_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
        }
    } else {
        if (!g_selftest) SaveNow();
        ShowWindow(g_note, SW_HIDE);
        PositionBall(true);
        ShowWindow(g_ball, SW_SHOWNOACTIVATE);
    }
}

static void ToggleNote() { SetNoteVisible(!g_noteVisible, kActivateOnShow); }

// ---- 自检（--selftest）----------------------------------------------------
static HWND g_zNote = nullptr, g_zTest = nullptr;
static int g_zCount = 0, g_zIdxNote = -1, g_zIdxTest = -1;

static BOOL CALLBACK EnumZ(HWND h, LPARAM) {
    if (h == g_zNote) g_zIdxNote = g_zCount;
    if (h == g_zTest) g_zIdxTest = g_zCount;
    ++g_zCount;
    return TRUE;
}

static bool NoteAbove(HWND test) {
    g_zNote = g_note; g_zTest = test;
    g_zCount = 0; g_zIdxNote = g_zIdxTest = -1;
    EnumWindows(EnumZ, 0);
    return g_zIdxNote >= 0 && g_zIdxTest >= 0 && g_zIdxNote < g_zIdxTest;
}

static void JBool(std::string& j, const char* key, bool v, bool last = false) {
    j += "  \""; j += key; j += "\": "; j += (v ? "true" : "false");
    if (!last) j += ",";
    j += "\n";
}

static void RunSelfTest() {
    // 1) UTF-8 / LF 往返（用独立文件，绝不碰用户的 note.txt）
    std::wstring sample = L"GSticky selftest 中文\r\n第二行\t缩进";
    std::string expect = NormalizeLf(ToUtf8(sample));
    bool writeOk = SaveNoteTo(g_selftestNotePath, sample);
    bool readOk = false;
    std::string back = ReadRawFile(g_selftestNotePath, readOk);
    bool utf8Ok = writeOk && readOk && back == expect &&
                  back.find('\r') == std::string::npos &&
                  FromUtf8(back) == L"GSticky selftest 中文\n第二行\t缩进";
    DeleteFileW(g_selftestNotePath.c_str());
    DeleteFileW((g_selftestNotePath + L".tmp").c_str());

    // 2) 层级：造一个「不遮任务栏的最大化窗口」，实测 z-order
    RECT wa = WorkAreaFor(ActiveMonitor());
    HWND test = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                L"Static", L"", WS_POPUP | WS_VISIBLE,
                                wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top,
                                nullptr, nullptr, g_hinst, nullptr);
    if (test) SetLayeredWindowAttributes(test, 0, 1, LWA_ALPHA);
    Sleep(100);  // 等 DWM 稳定 z-order
    bool above = test && NoteAbove(test);
    if (test) DestroyWindow(test);

    // 3) 状态与几何
    LONG noteEx = (LONG)GetWindowLongPtrW(g_note, GWL_EXSTYLE);
    LONG ballEx = (LONG)GetWindowLongPtrW(g_ball, GWL_EXSTYLE);
    bool topmost = (noteEx & WS_EX_TOPMOST) != 0;
    bool toolwindow = (noteEx & WS_EX_TOOLWINDOW) != 0;
    bool noActivate = (ballEx & WS_EX_NOACTIVATE) != 0;
    RECT nr = {};
    GetWindowRect(g_note, &nr);
    bool inside = nr.left >= wa.left && nr.top >= wa.top &&
                  nr.right <= wa.right && nr.bottom <= wa.bottom;
    bool editOk = g_edit && GetWindowTextLengthW(g_edit) >= 0;
    bool iconOk = g_iconsFromResource && g_iconBig && g_iconSmall;
    bool ballIconOk = g_iconsFromResource && g_iconBall != nullptr;

    // 4) 开机自启：在专用测试键上做「写 → 读 → 删 → 确认消失」完整往返。
    //    绝不读写用户真实的 Run 项，也不要求自启开关处于任何特定状态。
    std::wstring probe = RunCommand(ExePath());
    bool asWrite = probe.size() > 2 && WriteRegString(kSelftestKey, kSelftestValue, probe);
    std::wstring asRead;
    bool asReadOk = ReadRegString(kSelftestKey, kSelftestValue, asRead);
    bool asDelete = DeleteRegValue(kSelftestKey, kSelftestValue);
    std::wstring asAfter;
    bool asGone = !ReadRegString(kSelftestKey, kSelftestValue, asAfter);
    bool autostartRoundtrip = asWrite && asReadOk && asRead == probe && asDelete && asGone;
    RegDeleteKeyW(HKEY_CURRENT_USER, kSelftestKey);  // 单层键，只删自己创建的，不碰任何父键
    bool autostartEnabled = AutoStartEnabled();  // 只读真实状态，供诊断，不纳入 ok

    bool allOk = topmost && toolwindow && inside && above && editOk && g_hotkeyOk &&
                 g_trayOk && g_ball && noActivate && utf8Ok && iconOk && ballIconOk &&
                 autostartRoundtrip;
    std::string j = "{\n";
    j += "  \"ok\": " + std::string(allOk ? "true" : "false") + ",\n";
    j += "  \"mode\": \"selftest\",\n";
    j += "  \"dpi\": " + std::to_string(g_dpi) + ",\n";
    j += "  \"work_area\": [" + std::to_string(wa.left) + "," + std::to_string(wa.top) + "," +
         std::to_string(wa.right) + "," + std::to_string(wa.bottom) + "],\n";
    j += "  \"note_rect\": [" + std::to_string(nr.left) + "," + std::to_string(nr.top) + "," +
         std::to_string(nr.right) + "," + std::to_string(nr.bottom) + "],\n";
    JBool(j, "topmost", topmost);
    JBool(j, "toolwindow", toolwindow);
    JBool(j, "note_inside_work_area", inside);
    JBool(j, "note_above_maximized_window", above);
    JBool(j, "edit_control", editOk);
    JBool(j, "hotkey_registered", g_hotkeyOk);
    JBool(j, "tray_icon", g_trayOk);
    JBool(j, "ball_created", g_ball != nullptr);
    JBool(j, "ball_noactivate", noActivate);
    JBool(j, "utf8_lf_roundtrip", utf8Ok);
    JBool(j, "ball_icon_from_resource", ballIconOk);
    JBool(j, "icon_from_resource", iconOk);
    JBool(j, "autostart_registry_roundtrip", autostartRoundtrip);
    JBool(j, "autostart_enabled", autostartEnabled, true);
    j += "}\n";
    WriteRawFile(g_selftestOutPath, j);
}

// ---- 窗口过程 -------------------------------------------------------------
static void CleanupAndQuit() {
    KillTimer(g_note, kTimerSave);
    KillTimer(g_note, kTimerFlash);
    UnregisterHotKey(g_note, kHotkeyId);
    if (g_trayOk) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_ball) DestroyWindow(g_ball);
    if (g_font) DeleteObject(g_font);
    if (g_editBrush) DeleteObject(g_editBrush);
    if (g_iconsFromResource) DestroyIconTrio(g_iconBig, g_iconSmall, g_iconBall);
}

static LRESULT CALLBACK NoteProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);
        if ((HWND)lp == g_edit) {
            if (code == EN_CHANGE && !g_loading) {
                KillTimer(hwnd, kTimerSave);
                SetTimer(hwnd, kTimerSave, kSaveDebounceMs, nullptr);  // 0.5s debounce
            }
            return 0;
        }
        switch (id) {
        case kIdShow:     ToggleNote(); break;
        case kIdCopyAll:  FlashTrayTip(CopyAll() ? L"已复制全部（纯文本）" : L"复制失败"); break;
        case kIdAutoStart: {
            bool want = !AutoStartEnabled();  // 每次现读注册表，菜单不会记旧状态
            if (SetAutoStart(want))
                FlashTrayTip(want ? L"已开启开机自启" : L"已关闭开机自启");
            else
                FlashTrayTip(L"开机自启设置失败");
            break;
        }
        case kIdQuit:     if (!g_selftest) SaveNow(); DestroyWindow(hwnd); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerSave) {
            KillTimer(hwnd, kTimerSave);
            if (!g_selftest) SaveNow();
        } else if (wp == kTimerFlash) {
            KillTimer(hwnd, kTimerFlash);
            RestoreTrayTip();
        } else if (wp == kTimerSelftest) {
            KillTimer(hwnd, kTimerSelftest);
            RunSelfTest();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId) ToggleNote();
        return 0;
    case kMsgTray: {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK ||
            ev == kNinSelect || ev == kNinKeySelect) {
            ToggleNote();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    }
    case kMsgActivate:
        SetNoteVisible(true, kActivateOnShow);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, kNoteBg);
        SetTextColor(dc, kInk);
        return (LRESULT)g_editBrush;
    }
    case WM_SIZE:
        if (g_edit) {
            RECT cr = {};
            GetClientRect(hwnd, &cr);
            MoveWindow(g_edit, 0, 0, cr.right, cr.bottom, TRUE);
        }
        return 0;
    case WM_CLOSE:
        SetNoteVisible(false, false);  // 红叉 = 隐藏便签 + 显示悬浮球，与 macOS 一致
        return 0;
    case WM_QUERYENDSESSION:
        if (!g_selftest) SaveNow();
        return TRUE;
    case WM_DPICHANGED: {
        UINT d = HIWORD(wp);
        g_dpi = d ? d : 96;
        g_scale = (double)g_dpi / 96.0;
        RebuildFont();
        ReloadIconsOnDpi();
        RECT* suggested = (RECT*)lp;
        if (suggested)
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        int sz = S(kBaseBallSize);
        SetWindowPos(g_ball, HWND_TOPMOST, 0, 0, sz, sz,
                     SWP_NOMOVE | SWP_NOACTIVATE);
        SetWindowRgn(g_ball, CreateEllipticRgn(0, 0, sz + 1, sz + 1), TRUE);
        InvalidateRect(g_ball, nullptr, TRUE);  // 按新 DPI 重绘圆底与品牌标
        if (!g_noteVisible) PositionBall(true);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (g_noteVisible) PositionNote();       // 换显示器 / 改分辨率后拉回工作区
        else PositionBall(true);
        return 0;
    case WM_SETTINGCHANGE:
        if (!g_noteVisible) PositionBall(true);  // 切屏 / 任务栏变化后球跟着走
        return 0;
    case WM_DESTROY:
        if (!g_selftest) SaveNow();
        CleanupAndQuit();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK BallProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r = {};
        GetClientRect(hwnd, &r);
        // 圆底：系统窗口背景色 + 无描边，对齐 macOS BallView.draw
        // （原实现是写死黄色圆底 + 深色方块，与品牌 logo 完全无关）
        HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        HGDIOBJ oldBr = SelectObject(dc, br);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);
        DeleteObject(br);
        // 中心品牌云标：取自 exe 内 GSticky.ico，与窗口 / 托盘 / macOS 版同源
        int isz = S(kBaseBallIcon);
        int ix = (r.left + r.right - isz) / 2;
        int iy = (r.top + r.bottom - isz) / 2;
        if (g_iconBall)
            DrawIconEx(dc, ix, iy, g_iconBall, isz, isz, 0, nullptr, DI_NORMAL);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        g_ballDragging = true;
        g_ballMoved = false;
        GetCursorPos(&g_ballDragStart);
        RECT r = {};
        GetWindowRect(hwnd, &r);
        g_ballOriginStart.x = r.left;
        g_ballOriginStart.y = r.top;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_ballDragging) {
            POINT p = {};
            GetCursorPos(&p);
            int dx = p.x - g_ballDragStart.x, dy = p.y - g_ballDragStart.y;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            if (!g_ballMoved && (adx > S(3) || ady > S(3))) g_ballMoved = true;
            if (g_ballMoved)
                SetWindowPos(hwnd, HWND_TOPMOST,
                             g_ballOriginStart.x + dx, g_ballOriginStart.y + dy,
                             0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_ballDragging) {
            ReleaseCapture();
            g_ballDragging = false;
            if (g_ballMoved) PersistBallPos();
            else SetNoteVisible(true, kActivateOnShow);  // 点击唤出
        }
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 创建窗口 -------------------------------------------------------------
static bool CreateWindows() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NoteProc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kNoteClass;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW bc = {};
    bc.cbSize = sizeof(bc);
    bc.lpfnWndProc = BallProc;
    bc.hInstance = g_hinst;
    bc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    bc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  // 系统色画刷，与 WM_PAINT 圆底一致
    bc.lpszClassName = kBallClass;
    if (!RegisterClassExW(&bc)) return false;

    RECT wa = WorkAreaFor(ActiveMonitor());
    int nw = S(kBaseNoteW), nh = S(kBaseNoteH), nm = S(kNoteMargin);
    g_note = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kNoteClass, L"GSticky",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             wa.right - nw - nm, wa.top + nm, nw, nh,
                             nullptr, nullptr, g_hinst, nullptr);
    if (!g_note) return false;
    SendMessageW(g_note, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
    SendMessageW(g_note, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);

    RECT cr = {};
    GetClientRect(g_note, &cr);
    // 不加 ES_AUTOHSCROLL → 多行 Edit 自动软换行；ES_WANTRETURN → Enter 换行
    g_edit = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                             ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_NOHIDESEL,
                             0, 0, cr.right, cr.bottom,
                             g_note, (HMENU)1, g_hinst, nullptr);
    if (!g_edit) return false;
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(S(6), S(6)));
    RebuildFont();

    int bs = S(kBaseBallSize), bm = S(kBallMargin);
    g_ball = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                             kBallClass, L"GSticky 悬浮球", WS_POPUP,
                             wa.right - bs - bm, wa.top + bm, bs, bs,
                             nullptr, nullptr, g_hinst, nullptr);
    if (!g_ball) return false;
    SetLayeredWindowAttributes(g_ball, 0, kBallAlpha, LWA_ALPHA);
    SetWindowRgn(g_ball, CreateEllipticRgn(0, 0, bs + 1, bs + 1), TRUE);
    return true;
}

// ---- 入口 -----------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hinst = hInst;
    const wchar_t* cmd = GetCommandLineW();
    g_selftest = cmd && wcsstr(cmd, L"--selftest") != nullptr;

    if (!g_selftest) {  // 单实例：第二次运行只唤出已有便签
        HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND prev = FindWindowW(kNoteClass, nullptr);
            if (prev) PostMessageW(prev, kMsgActivate, 0, 0);
            return 0;
        }
        (void)mutex;
    }

    InitDpi();
    InitPaths();
    if (!g_selftest) RefreshAutoStartPath();  // 仅主实例、且开关已开时幂等自愈
    if (!LoadAppIcons()) {
        MessageBoxW(nullptr, L"GSticky 图标资源缺失，请用 win\\build.bat 重新构建。",
                    L"GSticky", MB_ICONERROR);
    }
    g_editBrush = CreateSolidBrush(kNoteBg);

    if (!CreateWindows()) {
        MessageBoxW(nullptr, L"GSticky 窗口创建失败。", L"GSticky", MB_ICONERROR);
        return 1;
    }
    if (!g_selftest) LoadNote();
    g_hotkeyOk = RegisterHotKey(g_note, kHotkeyId,
                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 0x47) != FALSE;
    InitTray();

    if (g_selftest) {
        ShowWindow(g_note, SW_SHOWNOACTIVATE);
        SetTimer(g_note, kTimerSelftest, 400, nullptr);
    } else {
        SetNoteVisible(true, false);  // 启动不抢焦点；热键/球/托盘唤出时才取焦点
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Win32 Edit 不自带 Ctrl+A，此处补齐；其余 Ctrl+C/V/X/Z 由控件处理
        if (msg.message == WM_KEYDOWN && msg.hwnd == g_edit &&
            (GetKeyState(VK_CONTROL) & 0x8000) && msg.wParam == 'A') {
            SendMessageW(g_edit, EM_SETSEL, 0, -1);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
