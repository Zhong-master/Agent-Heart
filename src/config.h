// ============================================================
// config.h — Agent-Heart 用户配置 (%LOCALAPPDATA%\Agent-Heart\config.json)
//
// 自实现极简 JSON 读写 (零第三方); 字段:
//   animationsEnabled  bool   默认 true   动效总开关 (关闭: 漂移/形变/相机飞行冻结)
//   sphereScale        float  默认 1.0    球形大小 0.5~1.5
//   clickEffectEnabled bool   默认 true   点击效果 (四环互换态)
//   mouseTrailEnabled  bool   默认 true   鼠标拖尾
// 目录与路径助手为 inline: app_data.cpp 直接引用不产生额外链接依赖。
// ============================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>

// 运行期配置 (全局单例, ConfigGet 取用)
struct AppConfig {
    bool  animationsEnabled = true;    // 动效总开关
    float sphereScale       = 1.0f;    // 球形缩放 (0.5 ~ 1.5)
    bool  clickEffectEnabled = true;   // 点击效果
    bool  mouseTrailEnabled  = true;   // 鼠标拖尾
};

// ---- 数据目录 / 文件路径 (inline, 供 config.cpp 与 app_data.cpp 共用) ----
// %LOCALAPPDATA%\Agent-Heart; 逐级 CreateDirectoryW (不存在则创建);
// LOCALAPPDATA 取不到时回退 exe 同目录; 目录创建失败仍返回路径 (写入方静默失败)。
inline std::wstring AgentHeartDataDir() {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) dir.assign(buf, n);
    if (dir.empty()) {                       // 兜底: exe 同目录
        wchar_t mod[MAX_PATH] = L"";
        const DWORD m = GetModuleFileNameW(nullptr, mod, MAX_PATH);
        if (m > 0 && m < MAX_PATH) {
            dir.assign(mod, m);
            const size_t p = dir.find_last_of(L"\\/");
            if (p != std::wstring::npos) dir.resize(p);
            else dir.clear();
        }
    }
    if (dir.empty()) return std::wstring();
    dir += L"\\Agent-Heart";
    // 逐级建目录 (从最短前缀到完整路径)
    for (size_t i = dir.find_first_of(L"\\/"); i != std::wstring::npos; ) {
        if (i > 2) CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);   // 跳过 "C:" 盘符
        i = dir.find_first_of(L"\\/", i + 1);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

inline std::wstring AgentHeartConfigPath() {          // %LOCALAPPDATA%\Agent-Heart\config.json
    const std::wstring d = AgentHeartDataDir();
    return d.empty() ? std::wstring() : d + L"\\config.json";
}

inline std::wstring AgentHeartStatePath() {           // %LOCALAPPDATA%\Agent-Heart\state.json
    const std::wstring d = AgentHeartDataDir();
    return d.empty() ? std::wstring() : d + L"\\state.json";
}

// ---- 配置存取 (启动加载 / 改动即存) ----
const AppConfig& ConfigGet();     // 当前配置 (Load 失败时为默认值)
void ConfigLoad();                // 启动时调用; 文件缺失/损坏 -> 保持默认
void ConfigSave();                // 写当前配置 (静默失败)
void ConfigSet(const AppConfig& c);   // 赋值 + 立即保存 (控件改动即保存)
void ConfigResetDefaults();       // 恢复默认值 + 保存
