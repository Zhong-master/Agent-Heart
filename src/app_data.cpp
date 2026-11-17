// ============================================================
// app_data.cpp — 原生数据层实现 (契约: src/app_data.h, 公共签名未改动)
//
// 逐段对照 dsh-bridge.ps1 十个端点语义 (docs/ui-port-spec.md §4):
//   LoadApps       = /apps.json + GET /categories
//                    (开始菜单两目录递归 .lnk / IShellLinkW 解析 /
//                     name|target 小写去重 / id 清洗_序号 / 7 内置入口 /
//                     cat-rules 关键字自动归类 + 状态文件覆盖 / 图标 96x96)
//   SetAppCategory = POST /categories   (单条 id->cat, 即改即存)
//   AddCategory    = POST /categories-list (空名/重名 false, 即改即存)
//   Launch         = /launch (ShellExecuteW; ms-settings: 协议同走 ShellExecuteW)
//   RefreshTasks   = /tasks (EnumWindows 可见顶层 -> taskCache_, 供 Focus/Close 复用)
//   QueryStats     = /sysinfo (PDH CPU/GPU 差分 + 内存 + 网卡字节差分 + 磁盘)
//   FocusTask      = /focus  (AttachThreadInput + ShowWindow + SetForegroundWindow)
//   CloseTask      = /close  (WM_CLOSE -> 500ms 未销毁 TerminateProcess 兜底)
// 持久化: exe 同目录 native-wallpaper-state.json (自实现极简 JSON, 无第三方库)。
// 所有失败/异常静默返回安全值; 中文一律宽字符, 落盘读取 UTF-8。
// ============================================================
#include "app_data.h"

#include <objbase.h>
#include <objidl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef STGM_READ
#define STGM_READ 0x00000000L
#endif
#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")

// shellapi.h 定义了 ExtractIcon -> ExtractIconW 宏, 会把同名成员函数展开坏;
// 本文件先包含 app_data.h (未展开) 再包含 shellapi.h, 此处取消宏以保护定义。
#undef ExtractIcon

namespace {

// ---------------- 宽字符 <-> UTF-8 / 大小写 ----------------
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

double R1(double v) { return std::round(v * 10.0) / 10.0; }
double R2(double v) { return std::round(v * 100.0) / 100.0; }

int64_t NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// ---------------- 状态文件: 自实现极简 JSON ----------------
// 只需要读 { "map": {id:cat}, "cats": [..] } 并对称写出; 兼容 \uXXXX 与代理对。
struct MiniJson {
    const std::string& s;
    size_t i = 0;
    explicit MiniJson(const std::string& t) : s(t) {
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            i = 3;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool take(char c) {
        ws();
        if (i < s.size() && s[i] == c) { i++; return true; }
        return false;
    }
    bool str(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return false;
                char e = s[i++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (!ok) return false;
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        unsigned lo = 0;
                        bool ok2 = true;
                        for (int k = 0; k < 4; k++) {
                            char h = s[i + 2 + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            else { ok2 = false; break; }
                        }
                        if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            i += 6;
                        }
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xF0 | (cp >> 18));
                        out += (char)(0x80 | ((cp >> 12) & 0x3F));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }
    bool skipValue() {
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"') { std::string t; return str(t); }
        if (c == '{') {
            i++;
            if (take('}')) return true;
            for (;;) {
                std::string k;
                if (!str(k)) return false;
                if (!take(':')) return false;
                if (!skipValue()) return false;
                if (take(',')) continue;
                if (take('}')) return true;
                return false;
            }
        }
        if (c == '[') {
            i++;
            if (take(']')) return true;
            for (;;) {
                if (!skipValue()) return false;
                if (take(',')) continue;
                if (take(']')) return true;
                return false;
            }
        }
        size_t st = i;
        while (i < s.size()) {
            char h = s[i];
            bool word = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'z') || (h >= 'A' && h <= 'Z') ||
                        h == '-' || h == '+' || h == '.' || h == '_';
            if (!word) break;
            i++;
        }
        return i > st;
    }
};

// 解析状态文件; 失败返回 false (调用方保持默认空状态, 同 bridge ConvertFrom-Json 失败)
bool ParseState(const std::string& text,
                std::unordered_map<std::string, std::wstring>& map,
                std::vector<std::wstring>& cats) {
    MiniJson j(text);
    if (!j.take('{')) return false;
    if (j.take('}')) return true;
    for (;;) {
        std::string key;
        if (!j.str(key)) return false;
        if (!j.take(':')) return false;
        if (key == "cats") {
            if (j.take('[')) {
                if (!j.take(']')) {
                    for (;;) {
                        std::string v;
                        if (j.str(v)) cats.push_back(Widen(v));
                        else if (!j.skipValue()) return false;
                        if (j.take(',')) continue;
                        if (j.take(']')) break;
                        return false;
                    }
                }
            } else {
                // 兼容标量
                std::string v;
                if (j.str(v)) cats.push_back(Widen(v));
                else if (!j.skipValue()) return false;
            }
        } else if (key == "map") {
            if (!j.take('{')) return false;
            if (!j.take('}')) {
                for (;;) {
                    std::string k;
                    if (!j.str(k)) return false;
                    if (!j.take(':')) return false;
                    std::string v;
                    if (j.str(v)) map[k] = Widen(v);
                    else if (!j.skipValue()) return false;
                    if (j.take(',')) continue;
                    if (j.take('}')) break;
                    return false;
                }
            }
        } else {
            if (!j.skipValue()) return false;
        }
        if (j.take(',')) continue;
        if (j.take('}')) return true;
        return false;
    }
}

void AppendJsonStr(std::string& o, const std::wstring& w) {
    const std::string u = Utf8(w);
    o += '"';
    for (unsigned char c : u) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                sprintf_s(b, "\\u%04x", (unsigned)c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    o += '"';
}

// 全局状态 (状态文件本身是单例; 公共签名不动, 用文件级静态承载)
std::unordered_map<std::string, std::wstring> g_stateMap;   // 用户覆盖 id -> cat
bool g_stateLoaded = false;

// ---------------- cat-rules.json 内嵌默认自动归类规则 ----------------
struct CatRule {
    const wchar_t* cat;
    std::vector<const wchar_t*> keys;
};

const std::vector<CatRule>& Rules() {
    static const std::vector<CatRule> r = {
        { L"游戏", { L"game", L"games", L"steam", L"xbox", L"minecraft", L"roblox", L"solitaire",
                    L"candy", L"league", L"valorant", L"genshin", L"pubg", L"游戏", L"我的世界",
                    L"纸牌", L"原神", L"崩坏", L"王者", L"吃鸡", L"棋牌" } },
        { L"娱乐", { L"media", L"player", L"music", L"video", L"photo", L"camera", L"movie",
                    L"netflix", L"spotify", L"youtube", L"bilibili", L"播放", L"音乐", L"视频",
                    L"照片", L"相机", L"电影", L"哔哩", L"网易云", L"爱奇艺", L"娱乐", L"录音" } },
        { L"系统", { L"settings", L"setting", L"control panel", L"task manager", L"device manager",
                    L"registry", L"regedit", L"services", L"event viewer", L"defender", L"firewall",
                    L"windows update", L"disk management", L"msconfig", L"system restore", L"backup",
                    L"printers", L"电源", L"设置", L"控制面板", L"任务管理", L"设备管理", L"注册表",
                    L"服务", L"事件查看", L"防火墙", L"更新", L"磁盘管理", L"备份", L"打印机",
                    L"系统", L"还原", L"计划任务" } },
        { L"工具", { L"notepad", L"paint", L"calculator", L"calc", L"terminal", L"powershell",
                    L"cmd", L"command", L"vscode", L"visual studio", L"code", L"7-zip", L"7zip",
                    L"winrar", L"zip", L"everything", L"listary", L"powertoys", L"remote desktop",
                    L"snip", L"clipboard", L"resource monitor", L"disk cleanup", L"defrag",
                    L"记事本", L"画图", L"计算器", L"终端", L"命令", L"代码", L"压缩", L"截图",
                    L"远程桌面", L"磁盘清理", L"工具" } },
    };
    return r;
}

// 同 bridge Get-AutoCat: 小写 contains, 规则顺序首个命中
std::wstring AutoCat(const std::wstring& name) {
    const std::wstring n = ToLower(name);
    for (const auto& r : Rules())
        for (const wchar_t* k : r.keys)
            if (n.find(k) != std::wstring::npos) return r.cat;
    return L"";
}

// ---------------- 图标: HICON -> straight RGBA (行优先) ----------------
// 实测 DrawIconEx 写入 32bpp DIB 时保留真实 alpha; 无 alpha 信息时用 AND 掩码兜底。
bool IconToRgba(HICON hicon, int px, std::vector<uint8_t>& out) {
    out.clear();
    if (!hicon || px <= 0) return false;
    ICONINFO ii{};
    const bool haveIi = GetIconInfo(hicon, &ii) != 0;
    bool ok = false;
    HDC hdc = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = px;
    bmi.bmiHeader.biHeight = -px;          // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HGDIOBJ dib = hdc ? CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (dib && bits) {
        memset(bits, 0, (size_t)px * px * 4);
        HGDIOBJ old = SelectObject(hdc, dib);
        DrawIconEx(hdc, 0, 0, hicon, px, px, 0, nullptr, DI_NORMAL);
        uint8_t* p = (uint8_t*)bits;
        const int n = px * px;
        int az = 0, aff = 0;
        for (int k = 0; k < n; k++) {
            const uint8_t a = p[k * 4 + 3];
            if (a == 0) az++;
            else if (a == 255) aff++;
        }
        if (az && aff) {
            // 真实 alpha 通道 (含 0/255 与半透明) 已写入 -> 直接采用
        } else {
            // 无 alpha 信息 (全 0 未写入 / 全 255 被压平): 用 AND 掩码生成二值 alpha
            bool masked = false;
            if (haveIi && ii.hbmMask) {
                BITMAP bm{};
                if (GetObjectW(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight >= bm.bmWidth) {
                    const int mw = bm.bmWidth, mh = bm.bmHeight;
                    const int andH = (mh < mw) ? mh : mw;   // 单色图标掩码高 2W, AND 占上半
                    const int stride = ((mw + 31) / 32) * 4;
                    std::vector<uint8_t> rows((size_t)stride * mh, 0);
                    BITMAPINFO mb{};
                    mb.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    mb.bmiHeader.biWidth = mw;
                    mb.bmiHeader.biHeight = mh;              // 正高度 = bottom-up
                    mb.bmiHeader.biPlanes = 1;
                    mb.bmiHeader.biBitCount = 1;
                    mb.bmiHeader.biCompression = BI_RGB;
                    HDC mdc = CreateCompatibleDC(nullptr);
                    if (mdc) {
                        if (GetDIBits(mdc, ii.hbmMask, 0, mh, rows.data(), &mb, DIB_RGB_COLORS)) {
                            masked = true;
                            for (int y = 0; y < px; y++) {
                                int my = (int)((int64_t)y * andH / px);
                                if (my >= andH) my = andH - 1;
                                const uint8_t* row = rows.data() + (size_t)(mh - 1 - my) * stride;
                                for (int x = 0; x < px; x++) {
                                    int mx = (int)((int64_t)x * mw / px);
                                    if (mx >= mw) mx = mw - 1;
                                    const int bit = (row[mx >> 3] >> (7 - (mx & 7))) & 1;
                                    p[((size_t)y * px + x) * 4 + 3] = bit ? 0 : 255;
                                }
                            }
                        }
                        DeleteDC(mdc);
                    }
                }
            }
            if (!masked && az == n) {
                for (int k = 0; k < n; k++) p[k * 4 + 3] = 255;   // 掩码不可得时保守视为不透明
            }
        }
        for (int k = 0; k < n; k++) {          // BGRA -> RGBA
            uint8_t* q = p + (size_t)k * 4;
            const uint8_t t = q[0];
            q[0] = q[2];
            q[2] = t;
        }
        out.assign(p, p + (size_t)n * 4);
        ok = true;
        SelectObject(hdc, old);
    }
    if (dib) DeleteObject(dib);
    if (hdc) DeleteDC(hdc);
    if (haveIi) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    return ok;
}

// ---------------- 开始菜单 .lnk 扫描 (同 bridge Get-Apps) ----------------
struct LnkEnt {
    std::wstring name, target, args, cwd;
    std::string id;
};

// 同 bridge: 清洗非 [a-zA-Z0-9_-] -> '_' (按 UTF-16 码元, 与 PS -replace 一致),
// 空结果补 'app'+序号, 再拼 '_' + 序号
std::string SanitizeId(const std::wstring& base, int index) {
    std::wstring id;
    id.reserve(base.size() + 8);
    for (wchar_t c : base) {
        const bool keep = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                          (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
        id += keep ? c : L'_';
    }
    if (id.empty()) id = L"app" + std::to_wstring(index);
    id += L"_" + std::to_wstring(index);
    return Utf8(id);
}

void AddLnk(IShellLinkW* sl, const std::wstring& lnkPath,
            std::vector<LnkEnt>& list, std::unordered_set<std::wstring>& seen) {
    if (!sl) return;
    // 加载 .lnk 走 IPersistFile::Load (IShellLink 本身无 Load)
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(__uuidof(IPersistFile), (void**)&pf)) || !pf) return;
    const HRESULT ld = pf->Load(lnkPath.c_str(), STGM_READ);
    pf->Release();
    if (FAILED(ld)) return;
    wchar_t buf[1024];
    std::wstring target, args, cwd;
    buf[0] = 0;
    if (SUCCEEDED(sl->GetPath(buf, 1024, nullptr, 0))) target = buf;
    buf[0] = 0;
    if (SUCCEEDED(sl->GetArguments(buf, 1024))) args = buf;
    buf[0] = 0;
    if (SUCCEEDED(sl->GetWorkingDirectory(buf, 1024))) cwd = buf;
    if (target.empty()) return;
    // 同 bridge: target 必须存在; explorer.exe 过滤
    const DWORD attr = GetFileAttributesW(target.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    const size_t slash = target.find_last_of(L"\\/");
    const std::wstring leaf = (slash == std::wstring::npos) ? target : target.substr(slash + 1);
    if (_wcsicmp(leaf.c_str(), L"explorer.exe") == 0) return;
    // name = .lnk 去扩展名 (PS BaseName)
    std::wstring base = lnkPath;
    if (base.size() >= 4) base.resize(base.size() - 4);
    const size_t bs = base.find_last_of(L"\\/");
    const std::wstring name = (bs == std::wstring::npos) ? base : base.substr(bs + 1);
    // 去重: (name|target) 小写
    if (!seen.insert(ToLower(name + L"|" + target)).second) return;
    LnkEnt e;
    e.name = name;
    e.target = target;
    e.args = args;
    e.cwd = cwd;
    e.id = SanitizeId(name, (int)list.size());
    list.push_back(std::move(e));
}

// 递归枚举 (同 Get-ChildItem -Recurse: 隐身/系统项默认不显示, 深度上限防环)
void ScanLnkDir(const std::wstring& dir, int depth, IShellLinkW* sl,
                std::vector<LnkEnt>& list, std::unordered_set<std::wstring>& seen) {
    if (depth > 16) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanLnkDir(full, depth + 1, sl, list, seen);
        } else {
            const size_t len = full.size();
            if (len >= 4 && _wcsicmp(full.c_str() + len - 4, L".lnk") == 0)
                AddLnk(sl, full, list, seen);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------- 系统指标采样 (持久 PDH 查询) ----------------
struct Sampler {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER cpu = nullptr;
    PDH_HCOUNTER gpu = nullptr;
    bool hasGpu = false;
    bool inited = false;
    uint64_t cpuTick = 0;      // 上次 CPU 采样 (GetTickCount64)
    double lastCpu = -1.0;
    double lastGpu = -1.0;
    uint64_t netTick = 0;      // 上次网卡基线 (0 = 尚无基线)
    uint64_t netIn = 0, netOut = 0;
    double lastRx = 0.0, lastTx = 0.0;
};

Sampler& GetSampler() {
    static Sampler s;
    return s;
}

// GPU Engine(*) 通配实例逐个取值取最大 (同 bridge GPUEngine 全表 max)
void ReadCpuGpu(Sampler& st) {
    if (st.cpu) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(st.cpu, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
            v.CStatus == ERROR_SUCCESS)
            st.lastCpu = R1(v.doubleValue);
    }
    if (st.gpu) {
        double mx = -1.0;
        DWORD size = 0, cnt = 0;
        const PDH_STATUS s1 = PdhGetFormattedCounterArrayW(st.gpu, PDH_FMT_DOUBLE, &size, &cnt, nullptr);
        if (size > 0 && (s1 == PDH_MORE_DATA || s1 == ERROR_SUCCESS)) {
            std::vector<PDH_FMT_COUNTERVALUE_ITEM> items(size);
            DWORD sz = size, c = 0;
            if (PdhGetFormattedCounterArrayW(st.gpu, PDH_FMT_DOUBLE, &sz, &c, items.data()) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < c && i < sz; i++) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > mx)
                        mx = items[i].FmtValue.doubleValue;
                }
            }
        }
        st.lastGpu = (mx >= 0.0) ? R1(mx) : 0.0;   // 计数器存在但无有效实例 -> 0; 计数器缺失 -> -1
    }
}

// 全部非回环网卡 64 位字节计数合计 (bridge: Perf Tcpip_NetworkInterface 全实例求和;
// Perf 的 Network Interface 不含软件回环, 故跳过 loopback)
bool ReadIfCounters(uint64_t& in, uint64_t& out) {
    MIB_IF_TABLE2* tab = nullptr;
    if (GetIfTable2(&tab) != NO_ERROR || !tab) return false;
    in = 0;
    out = 0;
    for (UINT i = 0; i < tab->NumEntries; i++) {
        const MIB_IF_ROW2& r = tab->Table[i];
        if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        in += r.InOctets;
        out += r.OutOctets;
    }
    FreeMibTable(tab);
    return true;
}

// ---------------- 任务窗口枚举 (EnumWindows 可见顶层主窗口) ----------------
struct TaskEnumCtx {
    DWORD selfPid = 0;
    DWORD onlyPid = 0;                      // >0: 只找该 pid 的主窗口
    HWND found = nullptr;
    std::vector<TaskItem>* out = nullptr;
    std::unordered_set<DWORD>* seenPid = nullptr;
};

// 进程名 (无扩展名) + 主模块图标 84x84
void FillTaskInfo(DWORD pid, TaskItem& t) {
    t.pid = pid;
    t.id = L"proc-" + std::to_wstring(pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring path;
    if (hp) {
        wchar_t buf[MAX_PATH] = L"";
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(hp, 0, buf, &n) && n > 0) path.assign(buf, n);
        CloseHandle(hp);
    }
    if (path.empty()) {
        t.name = std::to_wstring(pid);
        return;
    }
    const size_t s1 = path.find_last_of(L"\\/");
    std::wstring file = (s1 == std::wstring::npos) ? path : path.substr(s1 + 1);
    const size_t s2 = file.find_last_of(L'.');
    t.name = (s2 == std::wstring::npos || s2 == 0) ? file : file.substr(0, s2);
    // 图标: 主模块路径 ExtractIcon (失败回退 ExtractAssociatedIcon), 84x84
    static std::unordered_map<std::wstring, std::vector<uint8_t>> iconCache;
    auto it = iconCache.find(path);
    if (it == iconCache.end()) {
        std::vector<uint8_t> rgba;
        HICON big = nullptr;
        HICON h = nullptr;
        if (ExtractIconExW(path.c_str(), 0, &big, nullptr, 1) > 0 && big) {
            h = big;
        } else {
            WORD idx = 0;
            std::wstring mut = path;
            h = ExtractAssociatedIconW(GetModuleHandleW(nullptr), mut.data(), &idx);
        }
        if (h) {
            IconToRgba(h, 84, rgba);
            DestroyIcon(h);
        }
        it = iconCache.emplace(path, std::move(rgba)).first;
    }
    if (!it->second.empty()) {
        t.iconRgba = it->second;
        t.hasIcon = true;
        t.iconW = 84;
        t.iconH = 84;
    }
}

BOOL CALLBACK TaskEnumProc(HWND h, LPARAM lp) {
    try {
        auto* ctx = (TaskEnumCtx*)lp;
        if (!ctx) return TRUE;
        if (!IsWindowVisible(h)) return TRUE;
        if (GetWindowTextLengthW(h) <= 0) return TRUE;
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return TRUE;
        if (GetWindow(h, GW_OWNER)) return TRUE;            // 非主窗口 (同 bridge MainWindowHandle 口径)
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!pid) return TRUE;
        if (ctx->selfPid && pid == ctx->selfPid) return TRUE;
        if (ctx->onlyPid) {
            if (pid != ctx->onlyPid) return TRUE;
            ctx->found = h;
            return FALSE;
        }
        if (ctx->out && ctx->seenPid) {
            if (!ctx->seenPid->insert(pid).second) return TRUE;   // 每进程只取首个主窗口
            TaskItem t;
            wchar_t title[512] = L"";
            GetWindowTextW(h, title, 512);
            t.title = title;
            t.hwnd = h;
            FillTaskInfo(pid, t);
            ctx->out->push_back(std::move(t));
        }
    } catch (...) {
        // 回调不外抛
    }
    return TRUE;
}

HWND WindowOfPid(DWORD pid) {
    if (!pid) return nullptr;
    TaskEnumCtx ctx;
    ctx.onlyPid = pid;
    EnumWindows(TaskEnumProc, (LPARAM)&ctx);
    return ctx.found;
}

std::wstring LeafNoExt(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    std::wstring f = (s == std::wstring::npos) ? p : p.substr(s + 1);
    const size_t d = f.find_last_of(L'.');
    if (d != std::wstring::npos && d != 0) f.resize(d);
    return f;
}

// bridge /focus /close 的 app-id 路径: target 叶名 -> 进程 -> 首个有主窗口的进程
HWND WindowByTarget(const std::wstring& target) {
    const std::wstring leaf = LeafNoExt(target);
    if (leaf.empty()) return nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    HWND result = nullptr;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring pn = pe.szExeFile;
            const size_t d = pn.find_last_of(L'.');
            if (d != std::wstring::npos && d != 0) pn.resize(d);
            if (_wcsicmp(pn.c_str(), leaf.c_str()) == 0) {
                HWND wh = WindowOfPid(pe.th32ProcessID);
                if (wh) { result = wh; break; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

// id -> hwnd: proc-<pid> 走缓存/实时枚举; 否则按应用 target 找进程主窗口
HWND ResolveHwnd(const std::wstring& id, const std::vector<AppItem>& apps,
                 const std::vector<TaskItem>& cache) {
    if (id.empty()) return nullptr;
    if (id.rfind(L"proc-", 0) == 0) {
        const long v = wcstol(id.c_str() + 5, nullptr, 10);
        if (v <= 0) return nullptr;
        const DWORD pid = (DWORD)v;
        for (const auto& t : cache) {
            if (t.id == id && t.hwnd && IsWindow(t.hwnd)) {
                DWORD p = 0;
                GetWindowThreadProcessId(t.hwnd, &p);
                if (p == pid) return t.hwnd;
            }
        }
        return WindowOfPid(pid);
    }
    const std::string id8 = Utf8(id);
    for (const auto& a : apps)
        if (a.id == id8) return WindowByTarget(a.target);
    return nullptr;
}

}  // namespace

// ============================================================
// AppData 实现
// ============================================================

std::wstring AppData::StatePath() const {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) {
        dir.assign(buf, n);
        const size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir.resize(p + 1);
        else dir.clear();
    }
    return dir + L"native-wallpaper-state.json";
}

void AppData::SaveState() const {
    try {
        std::string o;
        o.reserve(256 + g_stateMap.size() * 48 + cats_.size() * 16);
        o += "{\"map\":{";
        bool first = true;
        for (const auto& kv : g_stateMap) {
            if (!first) o += ',';
            first = false;
            AppendJsonStr(o, Widen(kv.first));
            o += ':';
            AppendJsonStr(o, kv.second);
        }
        o += "},\"cats\":[";
        first = true;
        for (const auto& c : cats_) {
            if (!first) o += ',';
            first = false;
            AppendJsonStr(o, c);
        }
        o += "]}";
        HANDLE hf = CreateFileW(StatePath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;
        DWORD wr = 0;
        if (!o.empty()) WriteFile(hf, o.data(), (DWORD)o.size(), &wr, nullptr);
        CloseHandle(hf);
    } catch (...) {
        // 静默
    }
}

void AppData::ExtractIcon(AppItem& a, int px) const {
    a.hasIcon = false;
    a.iconW = 0;
    a.iconH = 0;
    a.iconRgba.clear();
    try {
        HICON h = nullptr;
        WORD idx = 0;
        if (a.target != L"ms-settings:" &&
            GetFileAttributesW(a.target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring path = a.target;   // ExtractAssociatedIconW 需要可写缓冲
            h = ExtractAssociatedIconW(GetModuleHandleW(nullptr), path.data(), &idx);
        }
        if (!h) {
            // 同 bridge: 无文件路径/提取失败 -> 回退 shell32 图标
            std::wstring sys = L"C:\\Windows\\System32\\shell32.dll";
            h = ExtractAssociatedIconW(GetModuleHandleW(nullptr), sys.data(), &idx);
        }
        if (!h) return;                     // 仍失败: hasIcon=false, UI 画渐变色块
        std::vector<uint8_t> rgba;
        const bool ok = IconToRgba(h, px, rgba);
        DestroyIcon(h);
        if (!ok || rgba.empty()) return;
        a.hasIcon = true;
        a.iconW = px;
        a.iconH = px;
        a.iconRgba = std::move(rgba);
    } catch (...) {
        a.hasIcon = false;
        a.iconRgba.clear();
    }
}

void AppData::LoadApps() {
    try {
        apps_.clear();
        // ---- 1) 状态文件 (同 bridge 启动时读 state.json) ----
        g_stateMap.clear();
        g_stateLoaded = false;
        cats_.clear();
        {
            HANDLE hf = CreateFileW(StatePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li{};
                if (GetFileSizeEx(hf, &li) && li.QuadPart > 0 && li.QuadPart < 4 * 1024 * 1024) {
                    std::string text((size_t)li.QuadPart, '\0');
                    DWORD rd = 0;
                    if (ReadFile(hf, text.data(), (DWORD)text.size(), &rd, nullptr) && rd == text.size()) {
                        text.resize(rd);
                        std::unordered_map<std::string, std::wstring> m;
                        std::vector<std::wstring> c;
                        if (ParseState(text, m, c)) {
                            g_stateMap = std::move(m);
                            cats_ = std::move(c);
                        }
                    }
                }
                CloseHandle(hf);
            }
            g_stateLoaded = true;
        }

        // ---- 2) 扫描两个开始菜单目录的 .lnk (同 bridge Get-Apps) ----
        std::vector<LnkEnt> list;
        std::unordered_set<std::wstring> seen;

        HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IShellLinkW* sl = nullptr;
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IShellLinkW), (void**)&sl);

        // ProgramData 目录同 bridge 写死; 用户目录取 %APPDATA%
        ScanLnkDir(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs", 0, sl, list, seen);
        wchar_t appData[MAX_PATH] = L"";
        const DWORD nEnv = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        if (nEnv > 0 && nEnv < MAX_PATH)
            ScanLnkDir(std::wstring(appData) + L"\\Microsoft\\Windows\\Start Menu\\Programs",
                       0, sl, list, seen);

        if (sl) sl->Release();

        // ---- 3) 内置系统入口 7 个 (同 bridge, 仅 name|target 未见过才追加) ----
        struct Builtin { const char* id; const wchar_t* name; const wchar_t* target; };
        static const Builtin kBuiltin[] = {
            { "sys-settings",   L"设置",         L"ms-settings:" },
            { "sys-control",    L"Control Panel", L"C:\\Windows\\System32\\control.exe" },
            { "sys-taskmgr",    L"Task Manager",  L"C:\\Windows\\System32\\taskmgr.exe" },
            { "sys-notepad",    L"Notepad",       L"C:\\Windows\\System32\\notepad.exe" },
            { "sys-paint",      L"Paint",         L"C:\\Windows\\System32\\mspaint.exe" },
            { "sys-calc",       L"Calculator",    L"C:\\Windows\\System32\\calc.exe" },
            { "sys-powershell", L"PowerShell",    L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" },
        };
        for (const auto& b : kBuiltin) {
            if (seen.count(ToLower(std::wstring(b.name) + L"|" + b.target))) continue;
            LnkEnt e;
            e.name = b.name;
            e.target = b.target;
            e.id = b.id;
            list.push_back(std::move(e));
        }

        // ---- 4) 组装: 分类 = 状态覆盖 > 关键字规则; 图标 96x96 (2x 密度) ----
        apps_.reserve(list.size());
        for (auto& e : list) {
            AppItem a;
            a.id = std::move(e.id);
            a.name = std::move(e.name);
            a.target = std::move(e.target);
            a.args = std::move(e.args);
            a.cwd = std::move(e.cwd);
            const auto it = g_stateMap.find(a.id);
            if (it != g_stateMap.end() && !it->second.empty()) a.cat = it->second;
            else a.cat = AutoCat(a.name);           // 同 bridge /apps.json: map 优先, 否则 Get-AutoCat
            apps_.push_back(std::move(a));
        }
        for (auto& a : apps_) ExtractIcon(a, 96);

        if (SUCCEEDED(ci)) CoUninitialize();
    } catch (...) {
        // 静默: 保留已扫描到的部分
    }
}

bool AppData::SetAppCategory(const std::string& appId, const std::wstring& cat) {
    try {
        if (appId.empty()) return false;
        if (!g_stateLoaded) LoadApps();
        if (cat.empty()) g_stateMap.erase(appId);
        else g_stateMap[appId] = cat;
        for (auto& a : apps_) {
            if (a.id == appId) {
                // 空分类 = 恢复自动归类 (同 bridge: 空 map 值回落 Get-AutoCat)
                a.cat = cat.empty() ? AutoCat(a.name) : cat;
                break;
            }
        }
        SaveState();                            // 同 bridge POST /categories 即时持久化
        return true;
    } catch (...) {
        return false;
    }
}

bool AppData::AddCategory(const std::wstring& name) {
    try {
        if (name.empty()) return false;                     // 空名 false
        for (const auto& c : cats_)
            if (c == name) return false;                    // 重名 false
        cats_.push_back(name);
        SaveState();                                        // 同 bridge POST /categories-list 即时持久化
        return true;
    } catch (...) {
        return false;
    }
}

bool AppData::Launch(const std::string& appId) {
    try {
        if (appId.empty()) return false;
        if (apps_.empty()) LoadApps();                      // 同 bridge Find-App -> Get-Apps
        const AppItem* app = nullptr;
        for (const auto& a : apps_)
            if (a.id == appId) { app = &a; break; }
        if (!app || app->target.empty()) return false;
        const wchar_t* dir = nullptr;
        if (!app->cwd.empty()) {
            const DWORD at = GetFileAttributesW(app->cwd.c_str());
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
                dir = app->cwd.c_str();                     // 同 bridge: cwd 存在才作为工作目录
        }
        // ms-settings: 协议与普通可执行文件统一走 ShellExecuteW
        const HINSTANCE hi = ShellExecuteW(nullptr, L"open", app->target.c_str(),
                                           app->args.empty() ? nullptr : app->args.c_str(),
                                           dir, SW_SHOWNORMAL);
        return (INT_PTR)hi > 32;
    } catch (...) {
        return false;
    }
}

void AppData::RefreshTasks(std::vector<TaskItem>& out) const {
    try {
        std::vector<TaskItem> v;
        std::unordered_set<DWORD> seenPid;
        TaskEnumCtx ctx;
        ctx.selfPid = GetCurrentProcessId();
        ctx.out = &v;
        ctx.seenPid = &seenPid;
        EnumWindows(TaskEnumProc, (LPARAM)&ctx);
        auto* self = const_cast<AppData*>(this);
        self->taskCache_ = std::move(v);                    // 供 Focus/Close 复用
        out = self->taskCache_;
    } catch (...) {
        out = taskCache_;                                   // 静默: 保留最近一次结果
    }
}

SysStats AppData::QueryStats() const {
    SysStats s;
    try {
        Sampler& st = GetSampler();
        if (!st.inited) {
            st.inited = true;
            if (PdhOpenQueryW(nullptr, 0, &st.q) == ERROR_SUCCESS) {
                if (PdhAddEnglishCounterW(st.q, L"\\Processor(_Total)\\% Processor Time", 0, &st.cpu) != ERROR_SUCCESS)
                    st.cpu = nullptr;
                if (PdhAddEnglishCounterW(st.q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &st.gpu) != ERROR_SUCCESS)
                    st.gpu = nullptr;
                st.hasGpu = (st.gpu != nullptr);            // 无 GPU 计数器类 -> gpu=-1 (同 bridge null)
            } else {
                st.q = nullptr;                             // CPU 采样失败 -> cpu=-1 (UI 显示 —)
            }
        }

        const uint64_t tick = GetTickCount64();
        const uint64_t gap = st.cpuTick ? (tick - st.cpuTick) : UINT64_MAX;

        if (gap > 3000) {
            // 首次 / 面板重开 (空档>3s): 先关闭旧窗口, 再取 400ms 新鲜差分 (口径 <=1s)
            if (st.q) PdhCollectQueryData(st.q);
            uint64_t in0 = 0, out0 = 0;
            const bool net0 = ReadIfCounters(in0, out0);
            Sleep(400);
            const uint64_t t2 = GetTickCount64();
            if (st.q) {
                PdhCollectQueryData(st.q);
                ReadCpuGpu(st);
            }
            uint64_t in1 = 0, out1 = 0;
            if (ReadIfCounters(in1, out1)) {
                const double sec = (t2 - tick) / 1000.0;
                if (net0 && sec >= 0.1) {
                    st.lastRx = R1((double)(in1 - in0) * 8.0 / 1000.0 / sec);
                    st.lastTx = R1((double)(out1 - out0) * 8.0 / 1000.0 / sec);
                }
                st.netIn = in1;
                st.netOut = out1;
                st.netTick = t2;
            }
            st.cpuTick = t2;
        } else if (gap >= 500) {
            // 正常 2s 轮询: CPU/GPU = 自上次采样的差分 (同 bridge PerfFormattedData 口径), 无阻塞
            if (st.q) {
                PdhCollectQueryData(st.q);
                ReadCpuGpu(st);
            }
            if (st.netTick > 0) {
                uint64_t in = 0, out = 0;
                if (ReadIfCounters(in, out)) {
                    const double sec = (tick - st.netTick) / 1000.0;
                    if (sec >= 0.5) {
                        const uint64_t din = (in >= st.netIn) ? (in - st.netIn) : 0;
                        const uint64_t dout = (out >= st.netOut) ? (out - st.netOut) : 0;
                        st.lastRx = R1((double)din * 8.0 / 1000.0 / sec);
                        st.lastTx = R1((double)dout * 8.0 / 1000.0 / sec);
                    }
                    st.netIn = in;
                    st.netOut = out;
                    st.netTick = tick;
                }
            }
            st.cpuTick = tick;
        }
        // gap < 500ms: 复用上次值 (避免过短窗口噪声)

        s.cpu = st.lastCpu;
        s.gpu = st.hasGpu ? ((st.lastGpu < 0.0) ? 0.0 : st.lastGpu) : -1.0;
        s.netRxKbps = st.lastRx;
        s.netTxKbps = st.lastTx;

        // 内存 (bridge: availGB 2 位 / totalGB 1 位, usedPct 用取整后值)
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            s.memTotalGB = R1((double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
            s.memAvailGB = R2((double)ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0));
            if (s.memTotalGB > 0.0)
                s.memUsedPct = R1((1.0 - s.memAvailGB / s.memTotalGB) * 100.0);
        }

        // 磁盘 C: (bridge: LogicalDisk C: FreeSpace/Size, 1 位)
        ULARGE_INTEGER ftc{}, tot{}, fre{};
        if (GetDiskFreeSpaceExW(L"C:\\", &ftc, &tot, &fre)) {
            s.diskTotalGB = R1((double)tot.QuadPart / (1024.0 * 1024.0 * 1024.0));
            s.diskFreeGB = R1((double)fre.QuadPart / (1024.0 * 1024.0 * 1024.0));
            if (s.diskTotalGB > 0.0)
                s.diskUsedPct = R1((1.0 - s.diskFreeGB / s.diskTotalGB) * 100.0);
        }

        s.tsMs = NowMs();
    } catch (...) {
        // 静默: 已采到的字段保留, 其余为安全默认值
    }
    return s;
}

bool AppData::FocusTask(const std::wstring& id) const {
    try {
        if (id.empty()) return false;
        if (id.rfind(L"proc-", 0) != 0 && apps_.empty())
            const_cast<AppData*>(this)->LoadApps();         // 同 bridge Find-App -> Get-Apps
        const HWND h = ResolveHwnd(id, apps_, taskCache_);
        if (!h || !IsWindow(h)) return false;
        DWORD fgTid = 0;
        const HWND fg = GetForegroundWindow();
        if (fg) fgTid = GetWindowThreadProcessId(fg, nullptr);
        const DWORD myTid = GetCurrentThreadId();
        bool attached = false;
        if (fgTid && fgTid != myTid)
            attached = AttachThreadInput(myTid, fgTid, TRUE) != 0;
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        else ShowWindow(h, SW_SHOW);
        bool ok = SetForegroundWindow(h) != 0;
        if (!ok) {
            // 兜底 A: 置顶 + SwitchToThisWindow (前台锁定时常用)
            BringWindowToTop(h);
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SwitchToThisWindow(h, TRUE);
            ok = SetForegroundWindow(h) != 0;
        }
        if (!ok) {
            // 兜底 B: 短暂 ALT 按键解除前台锁定 (SetForegroundWindow 经典绕法)
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = VK_MENU;
            SendInput(1, &in, sizeof(INPUT));
            ok = SetForegroundWindow(h) != 0;
            in.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
        }
        if (!ok && GetForegroundWindow() == h) ok = true;
        if (attached) AttachThreadInput(myTid, fgTid, FALSE);
        return ok;
    } catch (...) {
        return false;
    }
}

bool AppData::CloseTask(const std::wstring& id) const {
    try {
        if (id.empty()) return false;
        if (id.rfind(L"proc-", 0) != 0 && apps_.empty())
            const_cast<AppData*>(this)->LoadApps();
        const HWND h = ResolveHwnd(id, apps_, taskCache_);
        if (!h || !IsWindow(h)) return false;               // 找不到窗口 -> false
        PostMessageW(h, WM_CLOSE, 0, 0);                    // 优雅关闭
        for (int i = 0; i < 25; i++) {                      // 最多 500ms 等待销毁
            if (!IsWindow(h)) return true;
            Sleep(20);
        }
        if (!IsWindow(h)) return true;
        DWORD pid = 0;                                      // 兜底: 强制结束
        GetWindowThreadProcessId(h, &pid);
        if (pid) {
            HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hp) {
                TerminateProcess(hp, 1);
                CloseHandle(hp);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

uint32_t AppData::FallbackHue(const std::string& id) {
    uint32_t h = 2166136261u;                               // FNV-1a, 稳定哈希
    for (unsigned char c : id) {
        h ^= c;
        h *= 16777619u;
    }
    return h % 360u;
}

TaskItem* AppData::FindById(std::vector<TaskItem>& v, const std::wstring& id) const {
    for (auto& t : v)
        if (t.id == id) return &t;
    return nullptr;
}
