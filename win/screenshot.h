#pragma once
// GSticky Windows 区域截图子系统。
//
// 设计口径（见 README / win/VERIFICATION.md）：
//   - 只做「圈选 → 文字/画框/箭头 → 写剪贴板」，不做桌面文件、不做热键。
//   - 全程物理像素、虚拟屏坐标系；不复用主程序的 S()/L()（那是系统 DPI 逻辑像素）。
//   - 调用方（main.cpp）负责在 Start 前隐藏自家便签/悬浮球，在收到
//     kMsgShotDone 后恢复，并归还截屏前的前台窗口。
//   - 会话是主消息循环里的非阻塞状态机，不用嵌套 GetMessage 循环。
//   - SelfTestDib() 只在内存里做 DIB 往返，绝不创建窗口、绝不碰剪贴板。

#include <windows.h>

namespace gsticky_shot {

inline constexpr UINT kMsgShotDone = WM_APP + 3;  // wParam: 1=已复制, 0=取消/失败

// 启动一次截图会话。失败返回 false（调用方需自行恢复浮层）。
bool Start(HINSTANCE hinst, HWND owner);

// 会话进行中？
bool IsActive();

// 程序退出时强制收尾（不写剪贴板）。
void Cancel();

// 自检专用：32bpp top-down DIB 的内存往返，不碰任何用户数据。
bool SelfTestDib();

// 自检专用：纯内存复现覆盖层「压暗」路径，确认不会退化成全黑。
// 不建窗口、不抓屏、不碰剪贴板；取不到 AlphaBlend 直接判失败。
bool SelfTestDim();

}  // namespace gsticky_shot
