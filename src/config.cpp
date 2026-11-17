// ============================================================
// config.cpp — Agent-Heart 用户配置实现 (契约: src/config.h)
//   路径: %LOCALAPPDATA%\Agent-Heart\config.json (目录不存在则创建)
//   读: 自实现极简 JSON 扫描 (只认本文件自己写出的 4 个键, 容错跳过未知键)
//   写: 对称紧凑 JSON, float 保留 3 位; 所有失败静默 (保持现有值/默认值)
// ============================================================
#include "config.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

AppConfig g_cfg;              // 全局当前配置 (默认值即 AppConfig 成员初值)
bool g_loaded = false;        // Load 只执行一次语义 (重复调用允许, 幂等)

// ---------------- 极简 JSON 扫描 ----------------
// 跳过空白
void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// 在顶层对象里找 "key" 后面的字面量 (true/false/数字); 找到返回 true
bool FindScalar(const std::string& text, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    SkipWs(text, p);
    if (p >= text.size() || text[p] != ':') return false;
    ++p;
    SkipWs(text, p);
    if (p >= text.size()) return false;
    if (text[p] == '"') {                       // 字符串标量 (容错)
        ++p;
        out.clear();
        while (p < text.size() && text[p] != '"') out += text[p++];
        return p <= text.size();
    }
    size_t st = p;
    while (p < text.size()) {
        const char c = text[p];
        const bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') || c == '-' || c == '+' ||
                          c == '.' || c == 'e' || c == 'E';
        if (!word) break;
        ++p;
    }
    if (p == st) return false;
    out = text.substr(st, p - st);
    return true;
}

bool ParseBool(const std::string& v, bool def) {
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return def;
}

float ParseScale(const std::string& v, float def) {
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    if (end == v.c_str() || !std::isfinite(d)) return def;
    float f = static_cast<float>(d);
    if (f < 0.5f) f = 0.5f;                    // 范围 0.5 ~ 1.5
    if (f > 1.5f) f = 1.5f;
    return f;
}

// ---------------- 读文件 (失败返回空) ----------------
std::string ReadAll(const std::wstring& path) {
    std::string out;
    if (path.empty()) return out;
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER li{};
    if (GetFileSizeEx(hf, &li) && li.QuadPart > 0 && li.QuadPart < 1024 * 1024) {
        std::string t((size_t)li.QuadPart, '\0');
        DWORD rd = 0;
        if (ReadFile(hf, t.data(), (DWORD)t.size(), &rd, nullptr) && rd == t.size())
            out = std::move(t);
    }
    CloseHandle(hf);
    return out;
}

// ---------------- 写文件 (静默失败) ----------------
void WriteAll(const std::wstring& path, const std::string& data) {
    if (path.empty() || data.empty()) return;
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(hf, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(hf);
}

}  // namespace

const AppConfig& ConfigGet() { return g_cfg; }

void ConfigLoad() {
    try {
        const std::string text = ReadAll(AgentHeartConfigPath());
        if (text.empty()) {                     // 缺失/读失败 -> 默认值
            g_loaded = true;
            return;
        }
        std::string v;
        if (FindScalar(text, "animationsEnabled", v))
            g_cfg.animationsEnabled = ParseBool(v, g_cfg.animationsEnabled);
        if (FindScalar(text, "sphereScale", v))
            g_cfg.sphereScale = ParseScale(v, g_cfg.sphereScale);
        if (FindScalar(text, "clickEffectEnabled", v))
            g_cfg.clickEffectEnabled = ParseBool(v, g_cfg.clickEffectEnabled);
        if (FindScalar(text, "mouseTrailEnabled", v))
            g_cfg.mouseTrailEnabled = ParseBool(v, g_cfg.mouseTrailEnabled);
        g_loaded = true;
    } catch (...) {
        // 静默: 保持默认值
    }
}

void ConfigSave() {
    try {
        char buf[64];
        std::string o;
        o += "{\"animationsEnabled\":";
        o += g_cfg.animationsEnabled ? "true" : "false";
        o += ",\"sphereScale\":";
        snprintf(buf, sizeof(buf), "%.3f", (double)g_cfg.sphereScale);
        o += buf;
        o += ",\"clickEffectEnabled\":";
        o += g_cfg.clickEffectEnabled ? "true" : "false";
        o += ",\"mouseTrailEnabled\":";
        o += g_cfg.mouseTrailEnabled ? "true" : "false";
        o += "}";
        WriteAll(AgentHeartConfigPath(), o);
    } catch (...) {
        // 静默
    }
}

void ConfigSet(const AppConfig& c) {
    g_cfg = c;                                  // 钳范围 (调用方传入值可能越界)
    if (g_cfg.sphereScale < 0.5f) g_cfg.sphereScale = 0.5f;
    if (g_cfg.sphereScale > 1.5f) g_cfg.sphereScale = 1.5f;
    ConfigSave();                               // 改动即保存
}

void ConfigResetDefaults() {
    AppConfig d;                                // 成员初值 = 默认值
    ConfigSet(d);
}

void ConfigTouch() {                            // (保留: 让 g_loaded 被引用, 避免未用告警)
    (void)g_loaded;
}
