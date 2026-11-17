// ============================================================
// ui_overlay.h — 抽屉 + 任务面板覆盖层 (渲染 / 动效 / 命中测试 / 状态机)
//
// 权威规格: docs/ui-port-spec.md (8 章; 动效 A1-A18, 交互状态机 §3, 数据 §4)
// 坐标约定: 全部 DIP (与客户区 / sphere Resize 同单位); main 负责屏幕像素->DIP。
// 渲染集成: 由 SphereScene 的绘制钩子在场景之后调用 Draw(); dc 已带 DPI 变换,
//           单位 DIP; sceneSnapshot 为场景快照位图 (毛玻璃源, 仅覆盖层可见时非空)。
// ============================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1_1.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_data.h"

// 诊断日志通道 (main 注入; 未注入时 ui 静默)
extern std::function<void(const std::string&)> g_uiLog;

// 指针事件 (坐标 = 客户区 DIP)
enum class PtrKind { Move, Down, Up, DblClk, Wheel };
struct PointerEvent {
    PtrKind kind = PtrKind::Move;
    int button = 0;        // 0=左 1=右 2=中
    float x = 0, y = 0;
    int wheelDelta = 0;    // 仅 Wheel; <0 = 上滑 (开抽屉由 main 判定后调 OpenDrawer)
    bool down = false;     // Move 时: 是否有按键按住
};

class OverlayUI {
public:
    OverlayUI();
    ~OverlayUI();
    OverlayUI(const OverlayUI&) = delete;
    OverlayUI& operator=(const OverlayUI&) = delete;

    // ---- 顶层状态 (原 App.tsx Overlay = none|drawer|tasks) ----
    void OpenDrawer();          // 仅桌面滚轮上滑 (main 调用)
    void CloseDrawer();         // backdrop/Esc/拖拽>90/启动成功/Win+D
    void ToggleTaskPanel();     // 裸 Win 键
    void CloseAll();            // Win+D (kind=wind)
    bool DrawerOpen() const;    // 打开或动画中
    bool TasksOpen() const;
    bool AnyVisible() const;    // 任一覆盖层打开或动画中 -> main 每帧 SetSnapshotEnabled
    bool BlocksSphere() const;  // 打开时球体不接收指针 (原 pointer-events:auto)

    // ---- 输入路由; 返回 true = 已消费, main 不再转发球体 ----
    bool OnPointer(const PointerEvent& e);
    bool OnEsc();               // 打开时关闭顶层并消费
    // 键盘转发: main 的低级键盘钩子在覆盖层打开时调用; vk=虚拟键码,
    // ch=ToUnicodeEx 字符 (0=无), down=按下/抬起; 返回 true = 消费 (吞键, 供编辑模式文本输入)。
    bool OnKey(uint32_t vk, wchar_t ch, bool down);

    // ---- 帧驱动: 推进 tween/弹簧/拖拽回位, dt 秒 ----
    void Tick(double dt);

    // ---- 数据注入 (main 轮询: apps 首次打开抽屉时; tasks/stats 打开期间 2s) ----
    void SetApps(const std::vector<AppItem>& apps, const std::vector<std::wstring>& cats);
    void SetTasks(const std::vector<TaskItem>& tasks);
    void SetStats(const SysStats& stats);
    void SetDataError(bool appsUnavailable);   // 原 "启动桥未连接" 兜底态

    // ---- 动作回调 (main 注入) ----
    std::function<bool(const std::string& appId)> onLaunch;  // true=成功 -> UI 自动关抽屉
    std::function<void(const std::wstring& taskId)> onFocus; // 任务行点击
    std::function<void(const std::wstring& taskId)> onCloseTask; // ✕
    // 编辑模式持久化 (UI 本地乐观更新后调用; 失败时 UI 显示对应状态文案)
    std::function<bool(const std::string& appId, const std::wstring& cat)> onSetCategory;
    std::function<bool(const std::wstring& name)> onAddCategory;
    // 命中测试: 光标下是否有可交互控件 (main 用于光标形状), 坐标 = 客户区 DIP
    bool HitInteractive(float x, float y) const;

    // ---- 绘制 (SphereScene 钩子; 已带 DPI 变换, 单位 DIP) ----
    void Draw(ID2D1DeviceContext* dc, ID2D1Bitmap* sceneSnapshot, float dipW, float dipH);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
