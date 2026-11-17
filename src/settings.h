// ============================================================
// settings.h — Agent-Heart 设置窗口 (独立顶层原生窗口)
//
// 托盘右键 设置(S) 打开; 单实例 (已存在则置前)。
// 控件改动立即 ConfigSet 落盘, 由 main 每帧 OnFrameTick 同步到 SphereScene。
// ============================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// 打开设置窗口; 已有实例则 SetForegroundWindow 置前后返回。
void ShowSettingsWindow();
