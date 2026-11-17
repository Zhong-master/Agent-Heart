// ============================================================
// app_data.h — 原生数据层 (无 HTTP / 无 PowerShell / 无 HTML)
//
// 对应原桥接 (dsh-bridge.ps1) 的 10 个端点, 全部用 Win32/COM 原生实现:
//   /apps.json, /categories, /launch, POST /categories, POST /categories-list,
//   /sysinfo, /tasks, /focus, /close   (/winkey-state 由 main 的低级键盘钩子替代)
// 持久化: exe 同目录 native-wallpaper-state.json (自实现极简 JSON 读写)
// ============================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// 应用磁贴 (原 AppEntry {id,name,cat,icon})
struct AppItem {
    std::string id;        // 稳定 id (开始菜单名清洗 + "_序号", 规则同 bridge)
    std::wstring name;     // 显示名
    std::wstring target;   // 可执行路径或协议 (ms-settings: 等)
    std::wstring args;
    std::wstring cwd;
    std::wstring cat;      // 所属分类; 空 = 未分类
    // 图标: 48×48 straight RGBA (行优先, 4*48*48 字节); hasIcon=false 时 UI 画渐变色块
    bool hasIcon = false;
    int iconW = 0, iconH = 0;
    std::vector<uint8_t> iconRgba;
};

// 运行中任务 (原 TaskItem {id,name,title,pid,icon})
struct TaskItem {
    std::wstring id;       // "proc-<pid>" 或应用 id
    std::wstring name;     // 进程名/应用名
    std::wstring title;    // 主窗口标题
    uint32_t pid = 0;
    // 任务行 42px 图标 (straight RGBA); 与 AppItem 同规则, 可能为空
    bool hasIcon = false;
    int iconW = 0, iconH = 0;
    std::vector<uint8_t> iconRgba;
    HWND hwnd = nullptr;   // 主窗口 (focus/close 用)
};

// 系统五项指标 (原 SysInfo; -1 = 无数据, gpu=-1 = 无独立 GPU 计数器)
struct SysStats {
    double cpu = -1;
    double memUsedPct = -1;   double memTotalGB = 0; double memAvailGB = 0;
    double netRxKbps = 0;     double netTxKbps = 0;
    double diskUsedPct = -1;  double diskFreeGB = 0;  double diskTotalGB = 0;
    double gpu = -1;
    int64_t tsMs = 0;
};

class AppData {
public:
    // ---- 应用 (对应 /apps.json + /categories, 首次打开抽屉时加载一次) ----
    void LoadApps();                               // 扫描开始菜单 .lnk + 内置入口 + 图标提取 + 读状态文件
    const std::vector<AppItem>& Apps() const { return apps_; }
    const std::vector<std::wstring>& Categories() const { return cats_; }

    // 归类 (POST /categories 语义: 单条 id->cat, 立即持久化)
    bool SetAppCategory(const std::string& appId, const std::wstring& cat);
    // 新增分类 (POST /categories-list): 空名/重名返回 false
    bool AddCategory(const std::wstring& name);

    // 启动 (GET /launch): ShellExecuteW, 成功 true (UI 成功后关抽屉)
    bool Launch(const std::string& appId);

    // ---- 任务面板 (打开期间每 2s 刷新) ----
    void RefreshTasks(std::vector<TaskItem>& out) const;
    SysStats QueryStats() const;

    // GET /focus: 置前目标窗口 (SetForegroundWindow + AttachThreadInput 兜底)
    bool FocusTask(const std::wstring& id) const;
    // GET /close: 向主窗口发 WM_CLOSE (优雅), 失败兜底 terminate
    bool CloseTask(const std::wstring& id) const;

    // 磁贴 fallback 渐变色块的色相 (UI 用: 由 id 稳定哈希决定)
    static uint32_t FallbackHue(const std::string& id);

private:
    std::vector<AppItem> apps_;
    std::vector<std::wstring> cats_;
    // 状态文件路径 = exe 同目录 native-wallpaper-state.json
    std::wstring StatePath() const;
    void SaveState() const;
    void ExtractIcon(AppItem& a, int px) const;   // target -> iconRgba
    TaskItem* FindById(std::vector<TaskItem>& v, const std::wstring& id) const;
    std::vector<TaskItem> taskCache_;             // Focus/Close 复用最近一次枚举
};
