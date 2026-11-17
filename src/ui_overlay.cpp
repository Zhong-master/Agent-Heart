// ============================================================
// ui_overlay.cpp — 应用抽屉 + 任务面板覆盖层 (D2D / DWrite 纯原生)
//
// 权威规格: docs/ui-port-spec.md
//   §1 逐元素布局几何   §2 动效表 A1-A18   §3 交互状态机
//   §4 数据与兜底文案   §5 blur 参数        §6 图标 §7 字体
// 契约: src/ui_overlay.h (不修改), 数据类型: src/app_data.h
// 实现约定:
//   * 坐标全部 DIP; 绘制时按 "表面像素/DIP" 校正缩放 (s = px/dipW),
//     因此布局与命中测试始终在 DIP 空间进行。
//   * 毛玻璃 = D2D1GaussianBlur (stdDeviation = CSS blur 值, 边缘 HARD=clamp)
//     + D2D1ColorMatrix 精确饱和度矩阵 (D2D 饱和度效果上限 1, 不能过饱和)。
//   * 动效状态驱动: 自实现 cubic-bezier (Newton 迭代) + 通用 tween, 无 CSS。
// ============================================================
#include <initguid.h>
#include "ui_overlay.h"

#include <dwrite.h>
#include <d2d1effects.h>

std::function<void(const std::string&)> g_uiLog;   // main 注入的日志通道


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

using std::size_t;
using std::string;
using std::wstring;
using std::vector;

// =============================== COM 智能指针 ===============================
template <class T>
class CPtr {
public:
    CPtr() = default;
    ~CPtr() { Reset(); }
    CPtr(const CPtr&) = delete;
    CPtr& operator=(const CPtr&) = delete;
    CPtr(CPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    CPtr& operator=(CPtr&& o) noexcept {
        if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    void Attach(T* q) { Reset(); p_ = q; }
    T* Detach() { T* q = p_; p_ = nullptr; return q; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// =============================== 几何小工具 ===============================
inline D2D1_RECT_F R(float l, float t, float r, float b) {
    return D2D1_RECT_F{ l, t, r, b };
}
inline float RW(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float RH(const D2D1_RECT_F& r) { return r.bottom - r.top; }
inline bool PtIn(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
inline bool PtIn(const D2D1_RECT_F& r, const D2D1_POINT_2F& p) { return PtIn(r, p.x, p.y); }
inline D2D1_RECT_F ShiftY(const D2D1_RECT_F& r, float dy) {
    return R(r.left, r.top + dy, r.right, r.bottom + dy);
}
inline D2D1_RECT_F ShiftX(const D2D1_RECT_F& r, float dx) {
    return R(r.left + dx, r.top, r.right + dx, r.bottom);
}
inline D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d) {
    return R(r.left + d, r.top + d, r.right - d, r.bottom - d);
}
inline float Clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
// 正常行高 (Segoe UI normal line-height ≈ 1.33em); 显式 line-height 按 spec 传入
inline float LH(float s) { return s * 1.33f; }

// =============================== 缓动 (cubic-bezier) ===============================
struct CB {
    double x1, y1, x2, y2;
    double Bx(double t) const {
        double u = 1.0 - t;
        return 3.0 * u * u * t * x1 + 3.0 * u * t * t * x2 + t * t * t;
    }
    double By(double t) const {
        double u = 1.0 - t;
        return 3.0 * u * u * t * y1 + 3.0 * u * t * t * y2 + t * t * t;
    }
    double dBx(double t) const {
        double u = 1.0 - t;
        return 3.0 * u * u * x1 + 6.0 * u * t * (x2 - x1) + 3.0 * t * t * (1.0 - x2);
    }
    // Newton 迭代求 t (x(t)=p), 失败时二分兜底
    double operator()(double p) const {
        if (p <= 0.0) return 0.0;
        if (p >= 1.0) return 1.0;
        double t = p;
        for (int i = 0; i < 8; ++i) {
            double x = Bx(t) - p;
            if (std::fabs(x) < 1e-6) break;
            double d = dBx(t);
            if (std::fabs(d) < 1e-9) break;
            double nt = t - x / d;
            if (nt < 0.0 || nt > 1.0) break;
            t = nt;
        }
        if (std::fabs(Bx(t) - p) > 1e-4) {
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 24; ++i) {
                double m = (lo + hi) * 0.5;
                if (Bx(m) < p) lo = m; else hi = m;
            }
            t = (lo + hi) * 0.5;
        }
        return By(t);
    }
};

constexpr int kLinear = 0;
constexpr int kEase = 1;    // CSS ease = cubic-bezier(.25,.1,.25,1)
constexpr int kCb16 = 2;    // A2  cubic-bezier(.16,1,.3,1)
constexpr int kCb22 = 3;    // A13 cubic-bezier(.22,1,.36,1)

inline double EaseEval(int e, double p) {
    switch (e) {
    case kCb16: { static const CB c{ 0.16, 1.0, 0.3, 1.0 }; return c(p); }
    case kCb22: { static const CB c{ 0.22, 1.0, 0.36, 1.0 }; return c(p); }
    case kEase: { static const CB c{ 0.25, 0.1, 0.25, 1.0 }; return c(p); }
    default: return p;
    }
}

// =============================== 通用 tween ===============================
struct Anim {
    float from = 0.f, to = 0.f, dur = 1.f, t = 0.f;
    int ease = kEase;
    bool run = false;

    // 从当前值插值到 target (reverse: 再次 Start 反向目标即可)
    void Start(float target, float duration, int ez) {
        float cur = Value();
        from = cur;
        to = target;
        dur = duration < 1e-4f ? 1e-4f : duration;
        t = 0.f;
        ease = ez;
        run = (cur != target);
    }
    void Halt() {
        float v = Value();
        from = to = v;
        t = 0.f;
        run = false;
    }
    bool Tick(double dt) {
        if (!run) return false;
        t += (float)dt;
        if (t >= dur) { t = dur; run = false; return true; }
        return false;
    }
    float Value() const {
        if (!run) return to;
        float u = t / dur;
        if (u < 0.f) u = 0.f;
        if (u > 1.f) u = 1.f;
        return from + (to - from) * (float)EaseEval(ease, u);
    }
    bool Running() const { return run; }
};

// 悬停过渡: 两个时长 (transform 0.12s / 其余 0.15s, 见 A5-A9/A15-A17)
struct Hov {
    bool on = false;
    Anim fast, slow;
    void Set(bool now, float dFast, float dSlow) {
        if (now == on) return;
        on = now;
        fast.Start(now ? 1.f : 0.f, dFast, kEase);
        slow.Start(now ? 1.f : 0.f, dSlow, kEase);
    }
    void Tick(double dt) { fast.Tick(dt); slow.Tick(dt); }
    float Fast() const { return fast.Value(); }
    float Slow() const { return slow.Value(); }
};

// =============================== 颜色 ===============================
// 0xRRGGBBAA
inline D2D1_COLOR_F Hex(DWORD v) {
    return D2D1_COLOR_F{
        ((v >> 24) & 0xFF) / 255.f,
        ((v >> 16) & 0xFF) / 255.f,
        ((v >> 8) & 0xFF) / 255.f,
        (v & 0xFF) / 255.f,
    };
}
inline D2D1_COLOR_F WithA(const D2D1_COLOR_F& c, float a) {
    return D2D1_COLOR_F{ c.r, c.g, c.b, Clampf(a, 0.f, 1.f) };
}
inline D2D1_COLOR_F LerpC(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = Clampf(t, 0.f, 1.f);
    return D2D1_COLOR_F{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                         a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}
inline DWORD PackC(const D2D1_COLOR_F& c) {
    auto b = [](float f) -> DWORD {
        float v = Clampf(f, 0.f, 1.f) * 255.f + 0.5f;
        return (DWORD)v & 0xFF;
    };
    return (b(c.r) << 24) | (b(c.g) << 16) | (b(c.b) << 8) | b(c.a);
}
// HSL(h, s, l) -> RGBA (fallback 渐变色, spec §6.2)
inline D2D1_COLOR_F Hsl(float h, float s, float l, float a = 1.f) {
    h = std::fmod(h, 360.f);
    if (h < 0.f) h += 360.f;
    float c = (1.f - std::fabs(2.f * l - 1.f)) * s;
    float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
    float m = l - c * 0.5f;
    float r = 0.f, g = 0.f, b = 0.f;
    if (h < 60.f) { r = c; g = x; }
    else if (h < 120.f) { r = x; g = c; }
    else if (h < 180.f) { g = c; b = x; }
    else if (h < 240.f) { g = x; b = c; }
    else if (h < 300.f) { r = x; b = c; }
    else { r = c; b = x; }
    return D2D1_COLOR_F{ r + m, g + m, b + m, Clampf(a, 0.f, 1.f) };
}

// =============================== 字符串工具 ===============================
inline wstring WidenAscii(const char* s) {
    wstring o;
    if (!s) return o;
    for (const char* p = s; *p; ++p) o.push_back((wchar_t)(unsigned char)*p);
    return o;
}
inline wstring Widen(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return WidenAscii(s.c_str());
    wstring o((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n);
    return o;
}
inline string Narrow(const wstring& s) {
    if (s.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return string();
    string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}
inline wstring Trim(const wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}
inline bool Contains(const vector<wstring>& v, const wstring& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
// 码点边界 (代理对安全)
inline vector<size_t> CutPoints(const wstring& s) {
    vector<size_t> cuts;
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        wchar_t c = s[i];
        if ((c & 0xFC00) == 0xD800 && i + 1 < s.size() && (s[i + 1] & 0xFC00) == 0xDC00) n = 2;
        i += n;
        cuts.push_back(i);
    }
    return cuts;
}
// JS number -> 字符串 (统计卡数值格式, spec §4.3)
inline wstring NumFmt(double v) {
    if (std::isnan(v)) return L"NaN";
    if (std::isinf(v)) return v > 0 ? L"Infinity" : L"-Infinity";
    char buf[80];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        snprintf(buf, sizeof(buf), "%.0f", v);
        return WidenAscii(buf);
    }
    for (int p = 1; p <= 6; ++p) {
        snprintf(buf, sizeof(buf), "%.*f", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return WidenAscii(buf);
}
// 1 位小数 (mem sub 的 toFixed(1))
inline wstring Num1(double v) {
    if (std::isnan(v) || std::isinf(v)) return NumFmt(v);
    char buf[80];
    snprintf(buf, sizeof(buf), "%.1f", v);
    return WidenAscii(buf);
}

// =============================== 文本 (DWrite 单例) ===============================
class TextSys {
public:
    static TextSys& Get() { static TextSys t; return t; }

    // ha: 0=left 1=center 2=right; va: 0=top 1=center 2=bottom
    IDWriteTextFormat* Fmt(float size, int weight, int ha, int va) {
        if (!f_) return nullptr;
        std::uint64_t k = Key(size, weight, ha, va);
        auto it = fmts_.find(k);
        if (it != fmts_.end()) return it->second.Get();
        IDWriteTextFormat* tf = nullptr;
        if (FAILED(f_->CreateTextFormat(L"Segoe UI", nullptr,
                                        (DWRITE_FONT_WEIGHT)weight,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-CN", &tf)) || !tf)
            return nullptr;
        tf->SetTextAlignment((DWRITE_TEXT_ALIGNMENT)ha);
        tf->SetParagraphAlignment((DWRITE_PARAGRAPH_ALIGNMENT)va);
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmts_[k].Attach(tf);
        return fmts_[k].Get();
    }
    float Measure(const wstring& s, float size, int weight) {
        if (!f_ || s.empty()) return 0.f;
        auto* fmt = Fmt(size, weight, 0, 0);
        if (!fmt) return 0.f;
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(f_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 8000.f, 8000.f, &lay)) || !lay)
            return 0.f;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        float w = m.widthIncludingTrailingWhitespace;
        lay->Release();
        return w;
    }
    int Lines(const wstring& s, float maxW, float size, int weight) {
        if (!f_ || s.empty()) return 1;
        auto* fmt = Fmt(size, weight, 0, 0);
        if (!fmt) return 1;
        IDWriteTextLayout* lay = nullptr;
        float mw = maxW > 1.f ? maxW : 8000.f;
        if (FAILED(f_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, mw, 8000.f, &lay)) || !lay)
            return 1;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS m{};
        lay->GetMetrics(&m);
        int n = (int)m.lineCount;
        lay->Release();
        return n < 1 ? 1 : n;
    }
    // 单行省略 (spec: 任务标题 text-overflow: ellipsis)
    wstring Ellipsis(const wstring& s, float maxW, float size, int weight) {
        if (s.empty() || maxW <= 1.f) return wstring();
        if (Measure(s, size, weight) <= maxW) return s;
        vector<size_t> cuts = CutPoints(s);
        size_t lo = 0, hi = cuts.size();   // [0,hi) 内前缀合法
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            wstring t = s.substr(0, cuts[mid]) + L"\x2026";
            if (Measure(t, size, weight) <= maxW) lo = mid + 1;
            else hi = mid;
        }
        if (lo == 0) return L"\x2026";
        return s.substr(0, cuts[lo - 1]) + L"\x2026";
    }
    // 多行截断到 maxLines 行 (app-name line-clamp:2)
    wstring ClampLines(const wstring& s, float maxW, float size, int weight, int maxLines, int& outLines) {
        outLines = 1;
        if (s.empty()) return wstring();
        int n = Lines(s, maxW, size, weight);
        if (n <= maxLines) { outLines = n < 1 ? 1 : n; return s; }
        vector<size_t> cuts = CutPoints(s);
        size_t lo = 0, hi = cuts.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            wstring t = s.substr(0, cuts[mid]) + L"\x2026";
            if (Lines(t, maxW, size, weight) <= maxLines) lo = mid + 1;
            else hi = mid;
        }
        wstring out = (lo == 0) ? wstring(L"\x2026") : s.substr(0, cuts[lo - 1]) + L"\x2026";
        outLines = Lines(out, maxW, size, weight);
        if (outLines < 1) outLines = 1;
        if (outLines > maxLines) outLines = maxLines;
        return out;
    }

private:
    TextSys() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&f_))))
            f_ = nullptr;
    }
    static std::uint64_t Key(float size, int weight, int ha, int va) {
        std::uint64_t k = (std::uint64_t)(int)(size * 10.f + 0.5f) & 0xFFFFFFu;
        k |= ((std::uint64_t)weight & 0xFFFFu) << 24;
        k |= ((std::uint64_t)ha & 0xFu) << 44;
        k |= ((std::uint64_t)va & 0xFu) << 48;
        return k;
    }
    IDWriteFactory* f_ = nullptr;
    std::map<std::uint64_t, CPtr<IDWriteTextFormat>> fmts_;
};

}  // namespace

namespace {

// ==================== 命中测试结果 ====================
enum HitKind {
    kHitNone = 0,
    kHitBackdrop,     // 空白背板区 -> 点击关闭 (spec §3.2 backdrop 路径)
    kHitHead,         // 标题栏空白 -> 可发起拖拽
    kHitTabsStrip,    // tabs 容器空白 (可横向滚动)
    kHitTab, kHitEditBtn, kHitInput, kHitAddBtn, kHitEditHint,
    kHitTile, kHitSel, kHitStatus, kHitEmpty,
    kHitPopupItem, kHitPopupMiss,
    kHitRow, kHitClose, kHitCard, kHitSection, kHitList, kHitHint,
};
struct HitR {
    int k = kHitNone;
    int i = -1;
};

// ==================== 抽屉布局缓存 ====================
struct DrawerL {
    bool ok = false;
    float w = 0, h = 0;
    float headH = 0, bodyTop = 0;
    D2D1_RECT_F head{}, tabsClip{}, hintR{}, editBtn{}, handle{};
    vector<D2D1_RECT_F> tabR;      // x 相对 tabsClip.left, y 相对 tabsClip.top
    float tabContentW = 0, tabMax = 0;
    bool hasEditBar = false;
    D2D1_RECT_F editBar{}, inputR{}, addBtn{}, editHint{};
    D2D1_RECT_F body{};            // 正文可见裁剪区 (含 padding)
    float contentTop = 0;          // 内容原点屏幕 y (未滚动)
    float contentH = 0, scrollMax = 0;
    bool hasStatus = false;
    D2D1_RECT_F statusR{};         // 内容坐标
    bool hasEmpty = false;
    D2D1_RECT_F emptyR{};          // 内容坐标
    vector<D2D1_RECT_F> tileR;     // 内容坐标
    vector<D2D1_RECT_F> selR;      // 内容坐标 (编辑模式 select)
    float tileW = 0;
};

// ==================== 任务面板布局缓存 ====================
struct TaskL {
    bool ok = false;
    float w = 0, h = 0;
    vector<D2D1_RECT_F> cardR, labelR, valueR, subR, barR;
    vector<char> hasSub;           // 5 张卡
    D2D1_RECT_F statsRow{}, sectionR{}, list{}, hintR{}, emptyR{};
    bool hasEmpty = false;
    float contentTop = 0;          // 列表内容原点屏幕 y (未滚动)
    float contentH = 0, scrollMax = 0;
    vector<D2D1_RECT_F> rowR;      // x 绝对, y 相对内容原点
    vector<D2D1_RECT_F> closeR;
};

// ==================== 统计卡显示模型 (spec §4.3) ====================
struct StatUI {
    wstring label, value, sub;
    bool hasSub = false;
    D2D1_COLOR_F color{ 1, 1, 1, 1 };
    float pct = 0.f;
};

const wstring kBridgeMsg = L"启动桥未连接（dsh-bridge 未运行）";
const wstring kStatusLaunchFail = L"启动失败：";
const wstring kStatusCatFail = L"分类保存失败";
const wstring kStatusNewCatFail = L"新分类保存失败";

// 首字符 (spec §6: 磁贴取 name 首字符; 任务行取首字符并大写)
inline wstring FirstChar(const wstring& s) {
    if (s.empty()) return wstring();
    size_t n = 1;
    if ((s[0] & 0xFC00) == 0xD800 && s.size() > 1 && (s[1] & 0xFC00) == 0xDC00) n = 2;
    return s.substr(0, n);
}
inline wstring FirstCharUpper(const wstring& s) {
    wstring c = FirstChar(s);
    if (c.size() == 1) {
        wchar_t ch = c[0];
        if (ch >= L'a' && ch <= L'z') c[0] = (wchar_t)(ch - 32);
        else if (ch > 127) CharUpperW(&c[0]);     // 非 ASCII: 同 JS toUpperCase
    }
    return c;
}


}  // namespace

struct OverlayUI::Impl {
    OverlayUI* self_ = nullptr;

    // ---------------- 顶层状态 (App.tsx Overlay = none|drawer|tasks) ----------------
    enum class Top { None, Drawer, Tasks };
    Top state_ = Top::None;
    int pendingClose_ = 0;                    // 拖拽 >90 关闭: 延迟一帧 (spec §3.3)

    Anim aDpT_, aDpO_, aDpB_;                 // 抽屉 A2 transform / A3 opacity / A1 backdrop
    Anim aTkT_, aTkO_, aTkB_;                 // 任务面板 A13 transform / A12 opacity / A10+A11 backdrop

    bool dragging_ = false;                   // A4: 拖拽期间 transition none
    float dragStartY_ = 0.f, dragOffset_ = 0.f;
    bool dragSpring_ = false;                 // 松手回弹 (A2 弹簧)
    Anim dragBack_;

    // ---------------- 数据 ----------------
    vector<AppItem> apps_;
    vector<wstring> cats_;
    vector<TaskItem> tasks_;
    SysStats stats_{};
    bool statsValid_ = false;
    wstring status_;
    bool edit_ = false;
    wstring input_;
    bool inputFocus_ = false;
    wstring activeTab_ = L"全部";

    struct Grp { wstring name; vector<int> idx; };
    vector<Grp> groups_;
    int shown_ = 0;

    // ---------------- 绘制资源 (设备变化时整体丢弃) ----------------
    CPtr<ID2D1Device> dev_;
    CPtr<ID2D1Layer> layer_;
    CPtr<ID2D1Effect> fxBlurA_, fxSatA_, fxBlurB_, fxSatB_, fxBlurC_, fxBlurD_;
    std::map<std::uint32_t, CPtr<ID2D1SolidColorBrush>> brushes_;
    std::map<const void*, CPtr<ID2D1Bitmap>> iconsApp_;
    std::map<const void*, CPtr<ID2D1Bitmap>> iconsTask_;
    std::map<int, CPtr<ID2D1LinearGradientBrush>> grads_;
    ID2D1DeviceContext* dc_ = nullptr;
    float blurK_ = 1.f;                       // stdDeviation 校正 (DIP -> RT 空间)
    float w_ = 0, h_ = 0;

    // ---------------- 滚动 (阻尼跟随) ----------------
    float scroll_ = 0, scrollT_ = 0;
    float tabScroll_ = 0, tabScrollT_ = 0;
    float listScroll_ = 0, listScrollT_ = 0;

    // ---------------- 悬停过渡 ----------------
    vector<Hov> hovTab_;
    Hov hovEdit_, hovAdd_;
    vector<Hov> hovTile_, hovRow_, hovRowX_, hovPop_;

    // ---------------- 编辑模式分类下拉 (自绘 popup) ----------------
    bool popOpen_ = false;
    int popTile_ = -1;
    vector<wstring> popOpts_;

    // ---------------- 进度条 (A14, 0.6s 跟随) ----------------
    Anim bar_[5];
    float caretT_ = 0;                        // 输入框光标闪烁

    // ---------------- 布局缓存 ----------------
    bool layDirty_ = true;
    DrawerL dl_{};
    TaskL tl_{};

    // ==================== 数据辅助 ====================
    const vector<int>& ShownIdx() const {
        static const vector<int> kEmpty;
        if (shown_ < 0 || shown_ >= (int)groups_.size()) return kEmpty;
        return groups_[shown_].idx;
    }

    void RebuildGroups() {
        groups_.clear();
        Grp all;
        all.name = L"全部";
        all.idx.reserve(apps_.size());
        for (int i = 0; i < (int)apps_.size(); ++i) all.idx.push_back(i);
        groups_.push_back(std::move(all));
        for (const auto& c : cats_) {
            Grp g;
            g.name = c;
            for (int i = 0; i < (int)apps_.size(); ++i)
                if (apps_[i].cat == c) g.idx.push_back(i);
            if (!g.idx.empty()) groups_.push_back(std::move(g));   // 空分类过滤
        }
        Grp un;
        un.name = L"未分类";                                        // 恒显示
        for (int i = 0; i < (int)apps_.size(); ++i) {
            const auto& a = apps_[i];
            if (a.cat.empty() || !Contains(cats_, a.cat)) un.idx.push_back(i);
        }
        groups_.push_back(std::move(un));
        shown_ = 0;
        for (int i = 0; i < (int)groups_.size(); ++i)
            if (groups_[i].name == activeTab_) { shown_ = i; break; }
        if (shown_ >= (int)groups_.size()) shown_ = 0;
        layDirty_ = true;
    }

    void ClearHover() {
        for (auto& h : hovTab_) h.Set(false, 0.15f, 0.15f);
        hovEdit_.Set(false, 0.15f, 0.15f);
        hovAdd_.Set(false, 0.15f, 0.15f);
        for (auto& h : hovTile_) h.Set(false, 0.12f, 0.15f);
        for (auto& h : hovRow_) h.Set(false, 0.12f, 0.15f);
        for (auto& h : hovRowX_) h.Set(false, 0.15f, 0.15f);
        for (auto& h : hovPop_) h.Set(false, 0.15f, 0.15f);
    }

    void ClearPopup() {
        popOpen_ = false;
        popTile_ = -1;
        popOpts_.clear();
        hovPop_.clear();
    }

    void OpenPopup(int tile) {
        const auto& items = ShownIdx();
        if (tile < 0 || tile >= (int)items.size()) return;
        const AppItem& a = apps_[items[tile]];
        popOpts_.clear();
        if (!Contains(cats_, a.cat)) popOpts_.push_back(L"未分类");   // 选项顺序同 Drawer.tsx:223
        for (const auto& c : cats_) popOpts_.push_back(c);
        popOpen_ = true;
        popTile_ = tile;
        hovPop_.assign(popOpts_.size(), Hov{});
    }

    // popup 矩形: 随锚点 (select) 与滚动实时计算
    vector<D2D1_RECT_F> PopupItemRects() const {
        vector<D2D1_RECT_F> out;
        if (!popOpen_ || popTile_ < 0) return out;
        const DrawerL& L = dl_;
        if (!L.ok || edit_ == false) return out;
        if (popTile_ >= (int)L.tileR.size() || popTile_ >= (int)L.selR.size()) return out;
        float sc = Clampf(scroll_, 0.f, L.scrollMax);
        D2D1_RECT_F a = ShiftY(L.selR[popTile_], -sc);   // 内容坐标为 scroll=0 屏幕坐标
        float wpx = std::max(RW(a), 150.f);
        const float ih = 24.f;
        float hh = ih * (float)popOpts_.size() + 6.f;
        float x = a.left, y = a.bottom + 2.f;
        if (y + hh > h_ - 8.f) y = a.top - hh - 2.f;
        if (y < 8.f) y = 8.f;
        if (x + wpx > w_ - 8.f) x = std::max(8.f, w_ - 8.f - wpx);
        for (size_t i = 0; i < popOpts_.size(); ++i)
            out.push_back(R(x, y + 3.f + ih * (float)i, x + wpx, y + 3.f + ih * ((float)i + 1.f)));
        return out;
    }

    // ==================== 统计卡 (spec §4.3 表) ====================
    void BuildStats(StatUI out[5]) const {
        const wchar_t* labels[5] = { L"CPU", L"内存", L"网络", L"存储", L"GPU" };
        DWORD colors[5] = { 0x5b9dffff, 0xa78bfaff, 0x34d399ff, 0xfbbf24ff, 0xf472b6ff };
        for (int i = 0; i < 5; ++i) {
            out[i].label = labels[i];
            out[i].color = Hex(colors[i]);
            out[i].value = L"—";              // 缺数据一律 —
            out[i].sub.clear();
            out[i].hasSub = false;
            out[i].pct = 0.f;
        }
        if (!statsValid_) return;             // sys == null 的兜底态
        const SysStats& s = stats_;
        if (s.cpu >= 0) {
            out[0].value = NumFmt(s.cpu) + L"%";
            out[0].pct = (float)s.cpu;
        }
        if (s.memUsedPct >= 0 && s.memTotalGB > 0) {
            out[1].value = NumFmt(s.memUsedPct) + L"%";
            out[1].sub = Num1(s.memTotalGB - s.memAvailGB) + L"/" + NumFmt(s.memTotalGB) + L" GB";
            out[1].hasSub = true;
            out[1].pct = (float)s.memUsedPct;
        }
        out[2].value = L"↓" + NumFmt(s.netRxKbps) + L"  ↑" + NumFmt(s.netTxKbps);
        out[2].sub = L"Kbps";
        out[2].hasSub = true;
        out[2].pct = 0.f;                      // 网络进度条固定 0
        if (s.diskUsedPct >= 0) {
            out[3].value = NumFmt(s.diskUsedPct) + L"%";
            out[3].sub = L"剩 " + NumFmt(s.diskFreeGB) + L" GB";
            out[3].hasSub = true;
            out[3].pct = (float)s.diskUsedPct;
        }
        if (s.gpu >= 0) {
            out[4].value = NumFmt(s.gpu) + L"%";
            out[4].pct = (float)s.gpu;
        } else {
            out[4].sub = L"无独立 GPU";         // sys 存在但无 GPU 计数器
            out[4].hasSub = true;
        }
        for (int i = 0; i < 5; ++i) out[i].pct = Clampf(out[i].pct, 0.f, 100.f);
    }

    float BarTarget(int i) const {
        StatUI t[5];
        BuildStats(t);
        return t[i].pct;
    }

    // ==================== 面板变换 ====================
    // 抽屉: transform: translateY(48px) scale(.985) -> 0/1, 原点居中; 拖拽位移叠加
    D2D1_MATRIX_3X2_F DrawerM() const {
        float p = aDpT_.Value();
        float ty = (1.f - p) * 48.f + dragOffset_;
        float sc = 0.985f + 0.015f * p;
        float cx = w_ * 0.5f, cy = h_ * 0.5f;
        return D2D1::Matrix3x2F::Translation(-cx, -cy)
             * D2D1::Matrix3x2F::Scale(sc, sc)
             * D2D1::Matrix3x2F::Translation(0.f, ty)
             * D2D1::Matrix3x2F::Translation(cx, cy);
    }
    // 任务面板: transform: scale(.965) translateY(18px) -> 1/0
    D2D1_MATRIX_3X2_F TaskM() const {
        float p = aTkT_.Value();
        float ty = (1.f - p) * 18.f;
        float sc = 0.965f + 0.035f * p;
        float cx = w_ * 0.5f, cy = h_ * 0.5f;
        return D2D1::Matrix3x2F::Translation(-cx, -cy)
             * D2D1::Matrix3x2F::Translation(0.f, ty)
             * D2D1::Matrix3x2F::Scale(sc, sc)
             * D2D1::Matrix3x2F::Translation(cx, cy);
    }

    float DrawerAmt() const {
        float a = aDpT_.Value();
        a = std::max(a, aDpO_.Value());
        return std::max(a, aDpB_.Value());
    }
    float TasksAmt() const {
        float a = aTkT_.Value();
        a = std::max(a, aTkO_.Value());
        return std::max(a, aTkB_.Value());
    }

    // ==================== 顶层状态机 ====================
    void StartDragSpring() {
        dragBack_.from = dragOffset_;
        dragBack_.to = 0.f;
        dragBack_.dur = 0.52f;
        dragBack_.t = 0.f;
        dragBack_.ease = kCb16;               // 回弹走 A2 弹簧曲线
        dragBack_.run = (dragOffset_ > 0.01f);
        dragSpring_ = true;
    }

    void OpenDrawer() {
        if (state_ == Top::Drawer) return;
        state_ = Top::Drawer;
        aDpT_.Start(1.f, 0.52f, kCb16);       // A2
        aDpO_.Start(1.f, 0.52f, kEase);       // A3
        aDpB_.Start(1.f, 0.40f, kEase);       // A1
        aTkT_.Start(0.f, 0.30f, kCb22);
        aTkO_.Start(0.f, 0.26f, kEase);
        aTkB_.Start(0.f, 0.26f, kEase);
        pendingClose_ = 0;
        dragging_ = false;
        dragSpring_ = false;
        dragOffset_ = 0.f;
        ClearPopup();
        inputFocus_ = false;
        ClearHover();
        layDirty_ = true;
    }

    void CloseDrawer() {                       // 关闭路径: backdrop/Esc/拖拽>90/启动成功/CloseAll
        if (state_ != Top::Drawer) return;
        state_ = Top::None;
        pendingClose_ = 0;
        if (dragging_) { dragging_ = false; StartDragSpring(); }
        aDpT_.Start(0.f, 0.52f, kCb16);
        aDpO_.Start(0.f, 0.52f, kEase);
        aDpB_.Start(0.f, 0.40f, kEase);
        ClearPopup();
        inputFocus_ = false;
        ClearHover();
        layDirty_ = true;
    }

    void CloseTasks() {
        if (state_ != Top::Tasks) return;
        state_ = Top::None;
        aTkT_.Start(0.f, 0.30f, kCb22);
        aTkO_.Start(0.f, 0.26f, kEase);
        aTkB_.Start(0.f, 0.26f, kEase);
        ClearHover();
        layDirty_ = true;
    }

    void ToggleTaskPanel() {                   // 裸 Win 键
        if (state_ == Top::Tasks) { CloseTasks(); return; }
        state_ = Top::Tasks;
        aTkT_.Start(1.f, 0.30f, kCb22);        // A13
        aTkO_.Start(1.f, 0.26f, kEase);        // A12
        aTkB_.Start(1.f, 0.26f, kEase);        // A10/A11
        aDpT_.Start(0.f, 0.52f, kCb16);        // 抽屉反向收起
        aDpO_.Start(0.f, 0.52f, kEase);
        aDpB_.Start(0.f, 0.40f, kEase);
        pendingClose_ = 0;
        dragging_ = false;
        dragSpring_ = false;
        dragOffset_ = 0.f;
        ClearPopup();
        inputFocus_ = false;
        ClearHover();
        layDirty_ = true;
    }

    void CloseAll() {                          // Win+D (kind=wind)
        state_ = Top::None;
        pendingClose_ = 0;
        if (dragging_) { dragging_ = false; StartDragSpring(); }
        aDpT_.Start(0.f, 0.52f, kCb16);
        aDpO_.Start(0.f, 0.52f, kEase);
        aDpB_.Start(0.f, 0.40f, kEase);
        aTkT_.Start(0.f, 0.30f, kCb22);
        aTkO_.Start(0.f, 0.26f, kEase);
        aTkB_.Start(0.f, 0.26f, kEase);
        ClearPopup();
        inputFocus_ = false;
        ClearHover();
        layDirty_ = true;
    }

    bool OnEsc() {
        if (state_ == Top::Drawer) { CloseDrawer(); return true; }
        if (state_ == Top::Tasks) { CloseTasks(); return true; }
        return false;
    }

    bool OnKey(uint32_t vk, wchar_t ch, bool down) {
        if (state_ != Top::Drawer || !edit_ || !inputFocus_) return false;
        if (!down) {                            // 编辑态吞掉按键抬起
            return vk == VK_BACK || vk == VK_RETURN || vk == VK_ESCAPE || ch >= 32;
        }
        if (vk == VK_ESCAPE) return false;      // Esc 交给 OnEsc
        if (vk == VK_RETURN) { AddCategory(); return true; }
        if (vk == VK_BACK) {
            if (!input_.empty()) {
                // 代理对: 一次删一个码点
                size_t n = input_.size();
                if (n >= 2 && (input_[n - 2] & 0xFC00) == 0xD800 &&
                    (input_[n - 1] & 0xFC00) == 0xDC00)
                    input_.erase(n - 2, 2);
                else
                    input_.pop_back();
            }
            return true;
        }
        if (ch >= 32 && ch != 127) { input_ += ch; return true; }
        return false;
    }

    // ==================== 动作 ====================
    void Launch(const std::string& id) {
        bool ok = self_ && self_->onLaunch && self_->onLaunch(id);
        if (ok) {
            CloseDrawer();                      // 启动成功 -> 自动关抽屉
        } else {
            status_ = kStatusLaunchFail + Widen(id);
            layDirty_ = true;
        }
    }

    void AddCategory() {
        wstring name = Trim(input_);
        if (name.empty() || Contains(cats_, name)) return;   // 空/重名直接返回
        cats_.push_back(name);                               // 本地追加 (乐观)
        input_.clear();
        bool ok = !self_ || !self_->onAddCategory || self_->onAddCategory(name);
        if (!ok) status_ = kStatusNewCatFail;
        RebuildGroups();
        layDirty_ = true;
    }

    void ApplyCategory(int opt) {
        if (opt < 0 || opt >= (int)popOpts_.size()) return;
        const auto& items = ShownIdx();
        if (popTile_ < 0 || popTile_ >= (int)items.size()) return;
        int appIdx = items[popTile_];
        const wstring& v = popOpts_[opt];
        wstring cat = (v == L"未分类") ? wstring() : v;       // 未分类 -> 空 cat
        apps_[appIdx].cat = cat;                              // 本地乐观更新
        bool ok = !self_ || !self_->onSetCategory ||
                  self_->onSetCategory(apps_[appIdx].id, cat);
        if (!ok) status_ = kStatusCatFail;
        ClearPopup();
        RebuildGroups();
        layDirty_ = true;
    }

    // ==================== 拖拽 (spec §3.3) ====================
    void StartDrag(float y) {
        dragging_ = true;
        dragSpring_ = false;
        dragBack_.run = false;
        dragStartY_ = y;
        dragOffset_ = 0.f;                      // 行内 transform 重置为 0
    }
    void MoveDrag(float y) {
        if (!dragging_) return;
        float dy = y - dragStartY_;
        if (dy > 0.f) dragOffset_ = dy;         // 仅向下跟手 (dy<=0 不写 transform)
    }
    void EndDrag(float y) {
        if (!dragging_) return;
        dragging_ = false;
        float dy = y - dragStartY_;
        StartDragSpring();                      // 松手回弹 (A2)
        if (dy > 90.f) pendingClose_ = 1;       // >90px 延迟一帧关闭
    }

    // ==================== 帧驱动 ====================
    void Tick(double dt) {
        if (dt <= 0.0) return;
        if (dt > 0.1) dt = 0.1;
        if (pendingClose_ > 0) { if (--pendingClose_ == 0) CloseDrawer(); }
        aDpT_.Tick(dt); aDpO_.Tick(dt); aDpB_.Tick(dt);
        aTkT_.Tick(dt); aTkO_.Tick(dt); aTkB_.Tick(dt);
        if (dragSpring_) {
            dragBack_.Tick(dt);
            dragOffset_ = dragBack_.Value();
        }
        caretT_ += (float)dt;
        if (!dragging_) {                        // A4: 拖拽期间过渡冻结
            for (auto& h : hovTab_) h.Tick(dt);
            hovEdit_.Tick(dt); hovAdd_.Tick(dt);
            for (auto& h : hovTile_) h.Tick(dt);
            for (auto& h : hovRow_) h.Tick(dt);
            for (auto& h : hovRowX_) h.Tick(dt);
            for (auto& h : hovPop_) h.Tick(dt);
        }
        for (auto& b : bar_) b.Tick(dt);
        // 滚动阻尼 (指数趋近)
        const float k = 1.f - (float)std::exp(-dt * 16.0);
        scroll_ += (scrollT_ - scroll_) * k;
        tabScroll_ += (tabScrollT_ - tabScroll_) * k;
        listScroll_ += (listScrollT_ - listScroll_) * k;
        if (std::fabs(scroll_ - scrollT_) < 0.05f) scroll_ = scrollT_;
        if (std::fabs(tabScroll_ - tabScrollT_) < 0.05f) tabScroll_ = tabScrollT_;
        if (std::fabs(listScroll_ - listScrollT_) < 0.05f) listScroll_ = listScrollT_;
    }

    // ==================== 数据注入 ====================
    void SetApps(const vector<AppItem>& apps, const vector<wstring>& cats) {
        apps_ = apps;
        cats_ = cats;
        iconsApp_.clear();                      // 指针/世代失效
        if (status_ == kBridgeMsg) status_.clear();
        RebuildGroups();
        layDirty_ = true;
    }
    void SetTasks(const vector<TaskItem>& tasks) {
        tasks_ = tasks;
        iconsTask_.clear();
        layDirty_ = true;
    }
    void SetStats(const SysStats& stats) {
        stats_ = stats;
        statsValid_ = true;
        for (int i = 0; i < 5; ++i) {
            float tgt = BarTarget(i);
            if (std::fabs(tgt - bar_[i].Value()) > 0.05f)
                bar_[i].Start(tgt, 0.6f, kEase);          // A14: width 0.6s ease
        }
        layDirty_ = true;                                  // sub 显隐影响卡片高度
    }
    void SetDataError(bool bad) {
        if (bad) status_ = kBridgeMsg;
        else if (status_ == kBridgeMsg) status_.clear();
        layDirty_ = true;
    }

    // ==================== 布局 ====================
    float TileDy(int i) const {
        return (i >= 0 && i < (int)hovTile_.size()) ? hovTile_[i].Fast() : 0.f;
    }
    float RowDx(int i) const {
        return (i >= 0 && i < (int)hovRow_.size()) ? hovRow_[i].Fast() : 0.f;
    }

    void BuildDrawerLayout() {
        DrawerL L;
        const float w = w_, h = h_;
        if (w < 8.f || h < 8.f) { dl_ = L; return; }
        L.ok = true;
        L.w = w;
        L.h = h;
        auto& T = TextSys::Get();

        const float padX = 32.f;
        const float tabH = 14.f + LH(14.f);              // padding 7+7 + 行高
        const float btnH = 14.f + LH(13.f);
        const float rowH = std::max(tabH, btnH);
        L.headH = 22.f + rowH + 12.f + 1.f;              // padding 22/12 + border-bottom 1
        L.head = R(0.f, 0.f, w, L.headH);
        L.handle = R(w * 0.5f - 23.f, 7.f, w * 0.5f + 23.f, 12.f);   // 46x5, top 7

        // ---- 头部右侧 actions ----
        const wstring hintText = L"标题栏下拉 · Esc 或 Win+D 关闭";
        const wstring btnText = edit_ ? L"完成" : L"编辑";
        float hintW = T.Measure(hintText, 12.f, 400);
        float btnW = T.Measure(btnText, 13.f, 400) + 36.f;
        float actionsW = hintW + 14.f + btnW;
        float rowTop = 22.f;
        L.hintR = R(w - padX - actionsW, rowTop, w - padX - actionsW + hintW, rowTop + rowH);
        float btnY = rowTop + (rowH - btnH) * 0.5f;
        L.editBtn = R(w - padX - btnW, btnY, w - padX, btnY + btnH);

        // ---- tabs (flex 1, overflow-x, gap 10) ----
        float tabsRight = w - padX - actionsW - 18.f;    // head gap 18
        L.tabsClip = R(padX, rowTop, std::max(padX + 1.f, tabsRight), rowTop + rowH);
        float x = 0.f;
        float ty = (rowH - tabH) * 0.5f;
        for (int i = 0; i < (int)groups_.size(); ++i) {
            const auto& g = groups_[i];
            int wt = (i == shown_) ? 600 : 400;
            wstring cnt = std::to_wstring(g.idx.size());
            float tw = 36.f + T.Measure(g.name, 14.f, wt) + 7.f + T.Measure(cnt, 11.f, 400);
            L.tabR.push_back(R(x, ty, x + tw, ty + tabH));
            x += tw + 10.f;
        }
        L.tabContentW = x > 0.f ? x - 10.f : 0.f;
        L.tabMax = std::max(0.f, L.tabContentW - RW(L.tabsClip));

        // ---- 编辑工具条 ----
        if (edit_) {
            float inputH = 16.f + LH(13.f);              // padding 8+8
            float barTop = L.headH;
            float barH = 12.f + inputH + 12.f + 1.f;     // padding 12 + border-bottom 1
            L.hasEditBar = true;
            L.editBar = R(0.f, barTop, w, barTop + barH);
            L.inputR = R(padX, barTop + 12.f, padX + 260.f, barTop + 12.f + inputH);
            float addW = T.Measure(L"添加分类", 13.f, 400) + 36.f;
            float bY = barTop + 12.f + (inputH - btnH) * 0.5f;
            L.addBtn = R(L.inputR.right + 12.f, bY, L.inputR.right + 12.f + addW, bY + btnH);
            float ehW = T.Measure(L"在应用上选择分类即可归类", 12.f, 400);
            L.editHint = R(L.addBtn.right + 12.f, barTop + 12.f,
                           L.addBtn.right + 12.f + ehW, barTop + 12.f + inputH);
            L.bodyTop = barTop + barH;
        } else {
            L.bodyTop = L.headH;
        }
        L.body = R(0.f, L.bodyTop, w, h);

        // ---- 正文 (padding 22 32 40) ----
        float innerL = padX;
        float innerW = w - padX * 2.f;
        float cy = L.bodyTop + 22.f;
        L.contentTop = cy;
        if (!status_.empty()) {
            float boxH = 68.f + LH(14.f);
            L.hasStatus = true;
            L.statusR = R(innerL, cy, innerL + innerW, cy + boxH);
            cy += boxH;
        }

        // ---- 网格 repeat(auto-fill, minmax(116px,1fr)) gap 16 ----
        const auto& items = ShownIdx();
        int n = (int)items.size();
        int cols = (int)std::floor((innerW + 16.f) / (116.f + 16.f));
        if (cols < 1) cols = 1;
        L.tileW = (innerW - 16.f * (cols - 1)) / cols;
        if (L.tileW < 60.f) L.tileW = std::max(1.f, innerW);
        L.tileR.resize(n);
        L.selR.resize(n);
        int rows = (n + cols - 1) / cols;
        vector<float> rowHs((size_t)rows, 0.f);
        for (int i = 0; i < n; ++i) {
            const AppItem& a = apps_[items[(size_t)i]];
            float avail = L.tileW - 16.f;
            int ln = T.Lines(a.name, avail, 13.f, 500);
            if (ln < 1) ln = 1;
            if (ln > 2) ln = 2;                          // line-clamp: 2
            float th = 16.f + 50.f + 9.f + ln * (13.f * 1.3f) + 12.f;
            int rr = i / cols;
            rowHs[(size_t)rr] = std::max(rowHs[(size_t)rr], th);
        }
        float y = cy;
        for (int rr = 0; rr < rows; ++rr) {
            for (int c = 0; c < cols; ++c) {
                int i = rr * cols + c;
                if (i >= n) break;
                float x0 = innerL + c * (L.tileW + 16.f);
                L.tileR[(size_t)i] = R(x0, y, x0 + L.tileW, y + rowHs[(size_t)rr]);
                if (edit_) {
                    const float sh = 22.6f;               // select 高度
                    L.selR[(size_t)i] = R(x0 + 6.f, y + rowHs[(size_t)rr] - 6.f - sh,
                                          x0 + L.tileW - 6.f, y + rowHs[(size_t)rr] - 6.f);
                }
            }
            y += rowHs[(size_t)rr] + 16.f;
        }
        if (rows > 0) cy = y - 16.f;

        if (n == 0 && status_.empty()) {                 // 此分类暂无应用
            float boxH = 68.f + LH(14.f);
            L.hasEmpty = true;
            L.emptyR = R(innerL, cy, innerL + innerW, cy + boxH);
            cy += boxH;
        }
        L.contentH = cy - L.contentTop;
        float visible = h - L.bodyTop - 62.f;
        L.scrollMax = std::max(0.f, L.contentH - std::max(1.f, visible));
        dl_ = L;
    }

    void BuildTaskLayout() {
        TaskL L;
        const float w = w_, h = h_;
        if (w < 8.f || h < 8.f) { tl_ = L; return; }
        L.ok = true;
        L.w = w;
        L.h = h;

        const float padL = 56.f, padR = 56.f, padT = 46.f, padB = 26.f, gap = 18.f;
        float innerW = w - padL - padR;
        if (innerW < 200.f) innerW = std::max(1.f, w - 16.f);
        float cardW = (innerW - 16.f * 4.f) / 5.f;
        if (cardW < 40.f) cardW = 40.f;

        StatUI su[5];
        BuildStats(su);
        const float labelH = LH(13.f), valueH = 30.f * 1.15f, subH = LH(12.f), barH = 6.f;
        L.hasSub.assign(5, 0);
        vector<float> barY(5, 0.f);
        float cardH = 0.f;
        for (int i = 0; i < 5; ++i) {
            float y = 16.f + labelH + 6.f + valueH;      // padding-top 16, value margin-top 6
            L.hasSub[(size_t)i] = su[i].hasSub ? 1 : 0;
            if (su[i].hasSub) y += 3.f + subH;           // sub margin-top 3
            barY[(size_t)i] = y + 10.f;                  // bar margin-top 10
            cardH = std::max(cardH, barY[(size_t)i] + barH + 14.f);
        }
        L.cardR.resize(5); L.labelR.resize(5); L.valueR.resize(5);
        L.subR.resize(5); L.barR.resize(5);
        for (int i = 0; i < 5; ++i) {
            float x0 = padL + i * (cardW + 16.f);
            L.cardR[(size_t)i] = R(x0, padT, x0 + cardW, padT + cardH);
            L.labelR[(size_t)i] = R(x0 + 20.f, padT + 16.f, x0 + cardW - 20.f, padT + 16.f + labelH);
            float vy = padT + 16.f + labelH + 6.f;
            L.valueR[(size_t)i] = R(x0 + 20.f, vy, x0 + cardW - 20.f, vy + valueH);
            float sy = vy + valueH + 3.f;
            L.subR[(size_t)i] = R(x0 + 20.f, sy, x0 + cardW - 20.f, sy + subH);
            L.barR[(size_t)i] = R(x0 + 20.f, padT + barY[(size_t)i], x0 + cardW - 20.f,
                                  padT + barY[(size_t)i] + barH);
        }
        L.statsRow = R(padL, padT, w - padR, padT + cardH);

        // ---- 区块标题 (margin-top 6) 与列表 ----
        float secTop = padT + cardH + gap + 6.f;
        L.sectionR = R(padL, secTop, w - padR, secTop + LH(15.f));
        float listTop = secTop + LH(15.f) + gap;
        float hintH = LH(13.f);
        float listBottom = h - padB - hintH - gap;
        if (listBottom < listTop + 40.f) listBottom = listTop + 40.f;
        L.list = R(padL, listTop, w - padR, listBottom);
        L.hintR = R(padL, h - padB - hintH, w - padR, h - padB);
        L.contentTop = listTop;

        // ---- 任务行 (gap 12, rowH 70) ; 坐标为 scroll=0 时的屏幕坐标 ----
        int n = (int)tasks_.size();
        const float rowH = 70.f, rowGap = 12.f;
        L.rowR.resize((size_t)n);
        L.closeR.resize((size_t)n);
        if (n == 0) {
            L.hasEmpty = true;
            float boxH = 80.f + LH(14.f);                // padding 40 0
            L.emptyR = R(padL, listTop, w - padR, listTop + boxH);
            L.contentH = boxH;
        } else {
            for (int i = 0; i < n; ++i) {
                float y0 = listTop + i * (rowH + rowGap);
                L.rowR[(size_t)i] = R(padL, y0, w - padR, y0 + rowH);
                float cy0 = y0 + (rowH - 36.f) * 0.5f;
                L.closeR[(size_t)i] = R(w - padR - 20.f - 36.f, cy0, w - padR - 20.f, cy0 + 36.f);
            }
            L.contentH = n * rowH + (n - 1) * rowGap;
        }
        L.scrollMax = std::max(0.f, L.contentH - (listBottom - listTop));
        tl_ = L;
    }

    void EnsureLayout() {
        if (!layDirty_) return;
        BuildDrawerLayout();
        BuildTaskLayout();
        if (hovTab_.size() != groups_.size()) hovTab_.assign(groups_.size(), Hov{});
        if (hovTile_.size() != dl_.tileR.size()) hovTile_.assign(dl_.tileR.size(), Hov{});
        if (hovRow_.size() != tl_.rowR.size()) hovRow_.assign(tl_.rowR.size(), Hov{});
        if (hovRowX_.size() != tl_.rowR.size()) hovRowX_.assign(tl_.rowR.size(), Hov{});
        scroll_ = Clampf(scroll_, 0.f, dl_.scrollMax);
        scrollT_ = Clampf(scrollT_, 0.f, dl_.scrollMax);
        tabScroll_ = Clampf(tabScroll_, 0.f, dl_.tabMax);
        tabScrollT_ = Clampf(tabScrollT_, 0.f, dl_.tabMax);
        listScroll_ = Clampf(listScroll_, 0.f, tl_.scrollMax);
        listScrollT_ = Clampf(listScrollT_, 0.f, tl_.scrollMax);
        layDirty_ = false;
    }

    // ==================== 命中测试 (DIP, 面板局部坐标) ====================
    D2D1_POINT_2F ToLocal(Top t, float x, float y) const {
        D2D1_MATRIX_3X2_F m = (t == Top::Tasks) ? TaskM() : DrawerM();
        // 手写 2D 仿射逆 (D2D1_MATRIX_3X2_F 无 Inverse 成员)
        float det = m._11 * m._22 - m._21 * m._12;
        if (std::fabs(det) < 1e-12f) return D2D1_POINT_2F{ x, y };
        float a = m._22 / det, b = -m._21 / det;
        float c = -m._12 / det, d = m._11 / det;
        float n31 = -(a * m._31 + b * m._32);
        float n32 = -(c * m._31 + d * m._32);
        return D2D1_POINT_2F{ x * a + y * b + n31, x * c + y * d + n32 };
    }

    HitR HitDrawer(const D2D1_POINT_2F& p) const {
        const DrawerL& L = dl_;
        if (!L.ok) return HitR{};
        if (popOpen_) {
            vector<D2D1_RECT_F> items = PopupItemRects();
            for (size_t i = 0; i < items.size(); ++i)
                if (PtIn(items[i], p)) return HitR{ kHitPopupItem, (int)i };
            return HitR{ kHitPopupMiss, -1 };
        }
        if (PtIn(L.tabsClip, p)) {
            float ts = Clampf(tabScroll_, 0.f, L.tabMax);
            for (int i = 0; i < (int)L.tabR.size(); ++i) {
                D2D1_RECT_F r = ShiftX(L.tabR[(size_t)i], L.tabsClip.left - ts);
                r = ShiftY(r, L.tabsClip.top);
                if (PtIn(r, p)) return HitR{ kHitTab, i };
            }
            return HitR{ kHitTabsStrip, -1 };
        }
        if (PtIn(L.editBtn, p)) return HitR{ kHitEditBtn, -1 };
        if (L.hasEditBar) {
            if (PtIn(L.inputR, p)) return HitR{ kHitInput, -1 };
            if (PtIn(L.addBtn, p)) return HitR{ kHitAddBtn, -1 };
            if (PtIn(L.editHint, p)) return HitR{ kHitEditHint, -1 };
            if (PtIn(L.editBar, p)) return HitR{ kHitBackdrop, -1 };
        }
        if (PtIn(L.head, p)) return HitR{ kHitHead, -1 };   // 头部空白 = 拖拽热区
        if (PtIn(L.body, p)) {
            float sc = Clampf(scroll_, 0.f, L.scrollMax);
            if (L.hasStatus && PtIn(ShiftY(L.statusR, -sc), p)) return HitR{ kHitStatus, -1 };
            if (L.hasEmpty && PtIn(ShiftY(L.emptyR, -sc), p)) return HitR{ kHitEmpty, -1 };
            for (int i = 0; i < (int)L.tileR.size(); ++i) {
                D2D1_RECT_F r = ShiftY(L.tileR[(size_t)i], -sc - 4.f * TileDy(i));
                if (PtIn(r, p)) {
                    if (edit_ && i < (int)L.selR.size() &&
                        PtIn(ShiftY(L.selR[(size_t)i], -sc - 4.f * TileDy(i)), p))
                        return HitR{ kHitSel, i };
                    return HitR{ kHitTile, i };
                }
            }
            return HitR{ kHitBackdrop, -1 };                // 正文空白 -> backdrop 关闭
        }
        return HitR{ kHitBackdrop, -1 };                    // 面板其余空白 -> backdrop 关闭
    }

    HitR HitTasks(const D2D1_POINT_2F& p) const {
        const TaskL& L = tl_;
        if (!L.ok) return HitR{};
        float sc = Clampf(listScroll_, 0.f, L.scrollMax);
        if (PtIn(L.list, p)) {
            for (int i = 0; i < (int)L.rowR.size(); ++i) {
                float dx = 6.f * RowDx(i);
                D2D1_RECT_F rc = ShiftY(ShiftX(L.closeR[(size_t)i], dx), -sc);
                if (PtIn(rc, p)) return HitR{ kHitClose, i };
                D2D1_RECT_F rr = ShiftY(ShiftX(L.rowR[(size_t)i], dx), -sc);
                if (PtIn(rr, p)) return HitR{ kHitRow, i };
            }
            if (L.hasEmpty && PtIn(ShiftY(L.emptyR, -sc), p)) return HitR{ kHitEmpty, -1 };
            return HitR{ kHitList, -1 };
        }
        if (PtIn(L.statsRow, p)) return HitR{ kHitCard, -1 };
        if (PtIn(L.sectionR, p)) return HitR{ kHitSection, -1 };
        if (PtIn(L.hintR, p)) return HitR{ kHitHint, -1 };
        return HitR{ kHitBackdrop, -1 };
    }

    HitR HitAt(float x, float y, D2D1_POINT_2F* localOut) const {
        if (state_ == Top::None || w_ < 8.f || h_ < 8.f) return HitR{};
        const_cast<Impl*>(this)->EnsureLayout();
        Top t = state_;
        D2D1_POINT_2F p = ToLocal(t, x, y);
        if (localOut) *localOut = p;
        return (t == Top::Tasks) ? HitTasks(p) : HitDrawer(p);
    }

    void UpdateHover(const HitR& hr) {
        for (int i = 0; i < (int)hovTab_.size(); ++i)
            hovTab_[(size_t)i].Set(hr.k == kHitTab && hr.i == i, 0.15f, 0.15f);   // A5
        hovEdit_.Set(hr.k == kHitEditBtn, 0.15f, 0.15f);                          // A6
        hovAdd_.Set(hr.k == kHitAddBtn, 0.15f, 0.15f);                            // A6
        for (int i = 0; i < (int)hovTile_.size(); ++i)
            hovTile_[(size_t)i].Set(hr.k == kHitTile && hr.i == i, 0.12f, 0.15f); // A7/A8/A9
        for (int i = 0; i < (int)hovRow_.size(); ++i)
            hovRow_[(size_t)i].Set(hr.k == kHitRow && hr.i == i, 0.12f, 0.15f);   // A15/A16
        for (int i = 0; i < (int)hovRowX_.size(); ++i)
            hovRowX_[(size_t)i].Set(hr.k == kHitClose && hr.i == i, 0.15f, 0.15f);// A17
        for (int i = 0; i < (int)hovPop_.size(); ++i)
            hovPop_[(size_t)i].Set(hr.k == kHitPopupItem && hr.i == i, 0.15f, 0.15f);
    }

    // ==================== 输入路由 ====================
    void HandleWheel(const PointerEvent& e) {
        D2D1_POINT_2F p{};
        HitAt(e.x, e.y, &p);
        float d = (float)e.wheelDelta * 0.5f;             // <0 = 上滑 (web deltaY 语义)
        if (state_ == Top::Tasks) {
            const TaskL& L = tl_;
            if (!L.ok) return;
            if (PtIn(L.list, p))
                listScrollT_ = Clampf(listScrollT_ + d, 0.f, L.scrollMax);
        } else if (state_ == Top::Drawer) {
            const DrawerL& L = dl_;
            if (!L.ok) return;
            if (PtIn(L.tabsClip, p))
                tabScrollT_ = Clampf(tabScrollT_ + d, 0.f, L.tabMax);
            else if (PtIn(L.body, p))
                scrollT_ = Clampf(scrollT_ + d, 0.f, L.scrollMax);
        }
    }

    bool DownDrawer(const PointerEvent& e, const HitR& hr) {
        if (popOpen_) {                                  // popup 打开: 点外部先关闭并吞掉
            if (hr.k == kHitPopupItem) ApplyCategory(hr.i);
            ClearPopup();
            return true;
        }
        switch (hr.k) {
        case kHitTab:
            if (hr.i >= 0 && hr.i < (int)groups_.size()) {
                activeTab_ = groups_[(size_t)hr.i].name;
                RebuildGroups();
            }
            inputFocus_ = false;
            return true;
        case kHitEditBtn:
            edit_ = !edit_;
            if (!edit_) ClearPopup();
            inputFocus_ = false;
            layDirty_ = true;
            return true;
        case kHitInput:
            inputFocus_ = true;
            return true;
        case kHitAddBtn:
            inputFocus_ = false;
            AddCategory();
            return true;
        case kHitSel:
            inputFocus_ = false;
            OpenPopup(hr.i);
            return true;
        case kHitTile:
            inputFocus_ = false;
            if (!edit_) {                                // 编辑模式点击不启动
                const auto& items = ShownIdx();
                if (hr.i >= 0 && hr.i < (int)items.size())
                    Launch(apps_[(size_t)items[(size_t)hr.i]].id);
            }
            return true;
        case kHitHead:
            inputFocus_ = false;
            StartDrag(e.y);                              // 仅 .drawer-head 发起
            return true;
        case kHitEditHint:
        case kHitStatus:
        case kHitEmpty:
        case kHitTabsStrip:
            inputFocus_ = false;
            return true;
        default:
            inputFocus_ = false;
            CloseDrawer();                               // backdrop 点击关闭
            return true;
        }
    }

    bool DownTasks(const HitR& hr) {
        switch (hr.k) {
        case kHitClose:
            if (hr.i >= 0 && hr.i < (int)tasks_.size()) {
                TaskItem t = tasks_[(size_t)hr.i];
                if (self_ && self_->onCloseTask) self_->onCloseTask(t.id);
                tasks_.erase(tasks_.begin() + hr.i);      // 本地移除该行
                layDirty_ = true;
            }
            return true;
        case kHitRow:
            if (hr.i >= 0 && hr.i < (int)tasks_.size()) {
                const TaskItem& t = tasks_[(size_t)hr.i];
                if (self_ && self_->onFocus) self_->onFocus(t.id);
            }
            CloseTasks();                                 // focus 后无条件关闭面板
            return true;
        case kHitBackdrop:
            CloseTasks();                                 // backdrop 点击关闭
            return true;
        default:
            return true;
        }
    }

    bool OnPointer(const PointerEvent& e) {
        if (state_ == Top::None) return false;
        if (w_ < 8.f || h_ < 8.f) return true;            // 已打开但未布局: 消费不动作

        if (e.kind == PtrKind::Wheel) { HandleWheel(e); return true; }

        D2D1_POINT_2F local{};
        HitR hr = HitAt(e.x, e.y, &local);

        if (e.kind == PtrKind::Move) {
            if (dragging_ && state_ == Top::Drawer) { MoveDrag(e.y); return true; }
            UpdateHover(hr);
            return true;
        }
        if (e.kind == PtrKind::DblClk) return true;
        if (e.kind == PtrKind::Up) {
            if (dragging_) EndDrag(e.y);
            return true;
        }
        if (e.kind == PtrKind::Down) {
            if (dragging_) return true;
            if (state_ == Top::Tasks) return DownTasks(hr);
            return DownDrawer(e, hr);
        }
        return true;                                      // 覆盖层打开时全部消费
    }

    bool HitInteractive(float x, float y) const {
        HitR hr = HitAt(x, y, nullptr);
        switch (hr.k) {
        case kHitTab: case kHitEditBtn: case kHitInput: case kHitAddBtn:
        case kHitTile: case kHitSel: case kHitPopupItem:
        case kHitRow: case kHitClose:
            return true;
        default:
            return false;
        }
    }

    // ==================== 绘制: 基础原语 ====================
    bool layerOn_ = false;

    void ResetGpu() {
        brushes_.clear();
        iconsApp_.clear();
        iconsTask_.clear();
        grads_.clear();
        layer_.Reset();
        fxBlurA_.Reset(); fxSatA_.Reset();
        fxBlurB_.Reset(); fxSatB_.Reset();
        fxBlurC_.Reset(); fxBlurD_.Reset();
        layerOn_ = false;
    }

    ID2D1SolidColorBrush* Br(const D2D1_COLOR_F& c) {
        if (!dc_) return nullptr;
        DWORD k = PackC(c);
        auto it = brushes_.find(k);
        if (it != brushes_.end()) return it->second.Get();
        D2D1_COLOR_F cc = c;
        CPtr<ID2D1SolidColorBrush> b;
        if (FAILED(dc_->CreateSolidColorBrush(&cc, nullptr, b.Put())) || !b) return nullptr;
        auto& slot = brushes_[k];
        slot.Attach(b.Detach());
        return slot.Get();
    }

    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        if (c.a <= 0.0005f) return;
        auto* b = Br(c);
        if (b) dc_->FillRectangle(r, b);
    }
    void FillRR(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {
        if (c.a <= 0.0005f || RW(r) <= 0.f || RH(r) <= 0.f) return;
        auto* b = Br(c);
        if (!b) return;
        float rx = std::min(rad, RW(r) * 0.5f), ry = std::min(rad, RH(r) * 0.5f);
        D2D1_ROUNDED_RECT rr{ r, rx, ry };
        dc_->FillRoundedRectangle(rr, b);
    }
    void StrokeRR(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c, float sw = 1.f) {
        if (c.a <= 0.0005f || RW(r) <= sw || RH(r) <= sw) return;
        auto* b = Br(c);
        if (!b) return;
        D2D1_RECT_F ir = Inset(r, sw * 0.5f);
        float rx = std::min(rad, RW(ir) * 0.5f), ry = std::min(rad, RH(ir) * 0.5f);
        D2D1_ROUNDED_RECT rr{ ir, rx, ry };
        dc_->DrawRoundedRectangle(rr, b, sw);
    }

    void Txt(const wstring& s, const D2D1_RECT_F& r, float size, int weight,
             const D2D1_COLOR_F& c, int ha, int va) {
        if (s.empty() || !dc_ || RW(r) <= 0.f || RH(r) <= 0.f) return;
        auto* fmt = TextSys::Get().Fmt(size, weight, ha, va);
        auto* b = Br(c);
        if (!fmt || !b) return;
        dc_->DrawText(s.c_str(), (UINT32)s.size(), fmt, r, b,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    // letter-spacing 版本 (spec: .tp-stat-label 2px / .tp-section 3px / .tp-hint 3px)
    void TxtTracked(const wstring& s, const D2D1_RECT_F& r, float size, int weight,
                    const D2D1_COLOR_F& c, int ha, int va, float track) {
        if (s.empty() || !dc_) return;
        auto& T = TextSys::Get();
        vector<size_t> cuts = CutPoints(s);
        int n = (int)cuts.size();
        if (n == 0) return;
        float natural = T.Measure(s, size, weight);
        float total = natural + track * (float)(n - 1);
        float base = r.left;
        if (ha == 1) base = r.left + (RW(r) - total) * 0.5f;
        else if (ha == 2) base = r.right - total;
        float top = r.top;
        if (va == 1) top = r.top + (RH(r) - LH(size)) * 0.5f;
        else if (va == 2) top = r.bottom - LH(size);
        auto* b = Br(c);
        auto* fmt = T.Fmt(size, weight, 0, 0);
        if (!b || !fmt) return;
        size_t prev = 0;
        for (int i = 0; i < n; ++i) {
            size_t end = cuts[(size_t)i];
            wstring ch = s.substr(prev, end - prev);
            float x = base + T.Measure(s.substr(0, prev), size, weight) + track * (float)i;
            D2D1_RECT_F rc = R(x, top, r.right, top + LH(size));
            dc_->DrawText(ch.c_str(), (UINT32)ch.size(), fmt, rc, b,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            prev = end;
        }
    }

    // ---- 毛玻璃效果链 (spec §5) ----
    std::function<void(const std::string&)> g_uiLogImpl;   // 未用占位, 真通道见文件级

    static D2D1_MATRIX_5X4_F SatMatrix(float s) {
        const float lr = 0.213f, lg = 0.715f, lb = 0.072f;
        D2D1_MATRIX_5X4_F m{};
        m._11 = lr + (1.f - lr) * s; m._12 = lg * (1.f - s); m._13 = lb * (1.f - s);
        m._21 = lr * (1.f - s);      m._22 = lg + (1.f - lg) * s; m._23 = lb * (1.f - s);
        m._31 = lr * (1.f - s);      m._32 = lg * (1.f - s);      m._33 = lb + (1.f - lb) * s;
        m._44 = 1.f;
        return m;
    }

    void EnsureEffects(ID2D1Bitmap* snap) {
        if (!dc_ || !snap) return;
        auto mk = [&](CPtr<ID2D1Effect>& e, const GUID& id) {
            if (!e) dc_->CreateEffect(id, e.Put());
        };
        mk(fxBlurA_, CLSID_D2D1GaussianBlur);
        mk(fxSatA_, CLSID_D2D1ColorMatrix);
        mk(fxBlurB_, CLSID_D2D1GaussianBlur);
        mk(fxSatB_, CLSID_D2D1ColorMatrix);
        mk(fxBlurC_, CLSID_D2D1GaussianBlur);
        mk(fxBlurD_, CLSID_D2D1GaussianBlur);

        auto setBlur = [&](CPtr<ID2D1Effect>& e, float sd) {
            if (!e) return;
            const float v = std::max(0.f, sd);
            e->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, v);
            const D2D1_BORDER_MODE bm = D2D1_BORDER_MODE_HARD;   // 边缘 clamp
            e->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, bm);
        };
        auto setSat = [&](CPtr<ID2D1Effect>& e, float sv) {
            if (!e) return;
            D2D1_MATRIX_5X4_F m = SatMatrix(sv);
            e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
        };
        setBlur(fxBlurA_, 60.f * blurK_);                          // 抽屉 blur(60px) 恒定
        setBlur(fxBlurB_, 22.f * aTkB_.Value() * blurK_);          // 任务面板 0 -> 22px (A11)
        setBlur(fxBlurC_, 14.f * blurK_);                          // 统计卡 blur(14px)
        setBlur(fxBlurD_, 10.f * blurK_);                          // 任务行 blur(10px)
        if (fxBlurA_) fxBlurA_->SetInput(0, snap);
        if (fxBlurB_) fxBlurB_->SetInput(0, snap);
        if (fxSatA_ && fxBlurA_) fxSatA_->SetInputEffect(0, fxBlurA_.Get());
        if (fxSatB_ && fxBlurB_) fxSatB_->SetInputEffect(0, fxBlurB_.Get());
        if (fxBlurC_ && fxSatB_) fxBlurC_->SetInputEffect(0, fxSatB_.Get());
        if (fxBlurD_ && fxSatB_) fxBlurD_->SetInputEffect(0, fxSatB_.Get());
        setSat(fxSatA_, 1.6f);                                     // saturate(1.6)
        setSat(fxSatB_, 1.f + 0.2f * aTkB_.Value());               // saturate(1 -> 1.2) (A11)
    }

    void DrawEffectFull(ID2D1Effect* fx) {
        if (!fx || !dc_) return;
        ID2D1Image* img = nullptr;
        fx->GetOutput(&img);
        if (!img) return;
        D2D1_RECT_F full = R(0.f, 0.f, w_, h_);
        if (g_uiLog) {
            static int n = 0;
            if (n < 14) {
                ++n;
                D2D1_MATRIX_3X2_F t;
                dc_->GetTransform(&t);
                const char* tag = (fx == fxSatB_.Get()) ? "satB"
                    : (fx == fxBlurC_.Get()) ? "blurC"
                    : (fx == fxBlurD_.Get()) ? "blurD"
                    : (fx == fxSatA_.Get()) ? "satA" : "?";
                char buf[220];
                sprintf_s(buf, "fx/%s dest=(0,0,%.0f,%.0f) xform=(%.3f,%.3f,%.1f,%.1f)",
                    tag, w_, h_, t._11, t._22, t._31, t._32);
                g_uiLog(buf);
            }
        }
        if (g_uiLog) {
            static int m = 0;
            if (m < 8) {
                ++m;
                D2D1_MATRIX_3X2_F t;
                dc_->GetTransform(&t);
                char buf[200];
                sprintf_s(buf, "fx/atDraw dest=(%.0f,%.0f,%.0f,%.0f) xform=(%.4f %.4f %.1f %.1f)",
                    full.left, full.top, full.right, full.bottom,
                    t._11, t._22, t._31, t._32);
                g_uiLog(buf);
            }
        }
        // 实测: s=DIP 校正变换 + DIP 目标矩形绘制效果输出会产生 ×s 整体偏移
        // (球体团块落到 1950,1300 而非 1560,1040)。改为临时恒等变换 + 像素矩形,
        // 让效果输出 1:1 落位 (输入位图 dpi96, 像素即 DIP)。
        D2D1_MATRIX_3X2_F oldT;
        dc_->GetTransform(&oldT);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(oldT.dx, oldT.dy));
        D2D1_SIZE_U pxsz = dc_->GetPixelSize();
        D2D1_RECT_F fullPx = D2D1::RectF(0.f, 0.f,
            static_cast<float>(pxsz.width), static_cast<float>(pxsz.height));
        dc_->DrawImage(img, nullptr, &fullPx, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                       D2D1_COMPOSITE_MODE_SOURCE_OVER);
        dc_->SetTransform(oldT);
        img->Release();
    }

    // ---- 面板透明度层 (opacity 只作用于面板及其内容) ----
    void PushOpacity(float a) {
        if (!dc_) return;
        if (a >= 0.9995f) { layerOn_ = false; return; }
        if (!layer_) dc_->CreateLayer(layer_.Put());
        if (!layer_) { layerOn_ = false; return; }
        D2D1_LAYER_PARAMETERS1 lp{};
        lp.contentBounds = D2D1::InfiniteRect();
        lp.geometricMask = nullptr;
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::IdentityMatrix();
        lp.opacity = Clampf(a, 0.f, 1.f);
        lp.opacityBrush = nullptr;
        lp.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
        dc_->PushLayer(&lp, layer_.Get());
        layerOn_ = true;
    }
    void PopOpacity() {
        if (dc_ && layerOn_) { dc_->PopLayer(); layerOn_ = false; }
    }

    // ---- fallback 渐变 (spec §6.2: 135deg, 色相由 AppData::FallbackHue 决定) ----
    ID2D1LinearGradientBrush* Grad(int hue) {
        if (!dc_) return nullptr;
        auto it = grads_.find(hue);
        if (it != grads_.end()) return it->second.Get();
        D2D1_GRADIENT_STOP stops[2];
        stops[0].position = 0.f;
        stops[0].color = Hsl((float)hue, 0.94f, 0.67f, 1.f);            // #5b7cfa 同参
        stops[1].position = 1.f;
        stops[1].color = Hsl((float)hue + 30.f, 0.90f, 0.66f, 1.f);     // #8a5cf6 同参
        CPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(dc_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_CLAMP, sc.Put())) || !sc)
            return nullptr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp{};
        gp.startPoint = D2D1_POINT_2F{ 0.f, 0.f };
        gp.endPoint = D2D1_POINT_2F{ 1.f, 1.f };
        CPtr<ID2D1LinearGradientBrush> br;
        if (FAILED(dc_->CreateLinearGradientBrush(&gp, nullptr, sc.Get(), br.Put())) || !br)
            return nullptr;
        auto& slot = grads_[hue];
        slot.Attach(br.Detach());
        return slot.Get();
    }

    void DrawFallbackBox(const D2D1_RECT_F& r, float rad, const wstring& ch, int hue, float fs) {
        auto* g = Grad(hue);
        if (g && dc_) {
            g->SetStartPoint(D2D1_POINT_2F{ r.left, r.top });      // 135deg: 左上 -> 右下
            g->SetEndPoint(D2D1_POINT_2F{ r.right, r.bottom });
            float rx = std::min(rad, RW(r) * 0.5f), ry = std::min(rad, RH(r) * 0.5f);
            D2D1_ROUNDED_RECT rr{ r, rx, ry };
            dc_->FillRoundedRectangle(rr, g);
        } else {
            FillRR(r, rad, Hex(0x5b7cfaff));
        }
        Txt(ch, r, fs, 600, D2D1_COLOR_F{ 1.f, 1.f, 1.f, 1.f }, 1, 1);
    }

    // ---- 图标: straight RGBA -> premultiplied BGRA, 按指针缓存 ----
    ID2D1Bitmap* IconBmp(std::map<const void*, CPtr<ID2D1Bitmap>>& cache,
                         const vector<uint8_t>& rgba, int iw, int ih) {
        if (!dc_ || iw <= 0 || ih <= 0) return nullptr;
        if ((size_t)iw * (size_t)ih * 4 != rgba.size()) return nullptr;
        const void* key = rgba.data();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second.Get();
        vector<uint8_t> tmp(rgba.size());
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            unsigned a = rgba[i + 3];
            tmp[i + 0] = (uint8_t)((rgba[i + 2] * a + 127) / 255);
            tmp[i + 1] = (uint8_t)((rgba[i + 1] * a + 127) / 255);
            tmp[i + 2] = (uint8_t)((rgba[i + 0] * a + 127) / 255);
            tmp[i + 3] = (uint8_t)a;
        }
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f, 96.f);
        CPtr<ID2D1Bitmap> bmp;
        if (FAILED(dc_->CreateBitmap(D2D1::SizeU((UINT)iw, (UINT)ih), tmp.data(),
                                     (UINT32)(4 * iw), &props, bmp.Put())) || !bmp)
            return nullptr;
        auto& slot = cache[key];
        slot.Attach(bmp.Detach());
        return slot.Get();
    }

    void DrawIconImg(ID2D1Bitmap* bmp, int iw, int ih, const D2D1_RECT_F& dst) {
        if (!bmp || !dc_ || iw <= 0 || ih <= 0) return;
        float k = std::min(RW(dst) / (float)iw, RH(dst) / (float)ih);   // object-fit: contain
        float nw = iw * k, nh = ih * k;
        D2D1_RECT_F r = R(dst.left + (RW(dst) - nw) * 0.5f, dst.top + (RH(dst) - nh) * 0.5f,
                          dst.left + (RW(dst) + nw) * 0.5f, dst.top + (RH(dst) + nh) * 0.5f);
        dc_->DrawBitmap(bmp, r, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }

    void DrawBtn(const D2D1_RECT_F& r, const wstring& text, const Hov& hv, bool on) {
        float t = hv.Slow();
        D2D1_COLOR_F bg = on ? Hex(0xe8eaedff) : LerpC(Hex(0xffffff14), Hex(0xffffff29), t);
        D2D1_COLOR_F bd = on ? Hex(0xe8eaedff) : LerpC(Hex(0xffffff24), Hex(0xffffff4d), t);
        FillRR(r, 10.f, bg);
        StrokeRR(r, 10.f, bd);
        D2D1_COLOR_F fg = on ? Hex(0x101216ff) : Hex(0xd5dae2ff);
        Txt(text, r, 13.f, 400, fg, 1, 1);
    }

    void DrawVScroll(const D2D1_RECT_F& track, float sc, float contentH, float max) {
        if (max <= 0.05f || contentH <= 0.f || RW(track) < 12.f) return;
        float th = RH(track);
        float thumbH = std::max(30.f, th * th / contentH);
        if (thumbH > th) thumbH = th;
        float y = track.top + (sc / max) * (th - thumbH);
        FillRR(R(track.right - 10.f, y, track.right, y + thumbH), 5.f, Hex(0xffffff38));
    }

    // ==================== 抽屉绘制 ====================
    void DrawDrawer(ID2D1Bitmap* snap, float s, const D2D1_MATRIX_3X2_F& base) {
        if (!dc_) return;
        const float w = w_, h = h_;
        float aB = aDpB_.Value(), aO = aDpO_.Value();
        D2D1_MATRIX_3X2_F scM = D2D1::Matrix3x2F::Scale(s, s);

        // 1) .drawer-backdrop: rgba(0,0,0,0 -> .35) A1 0.4s ease, 无 transform
        dc_->SetTransform(base * scM);
        if (aB > 0.001f) Fill(R(0.f, 0.f, w, h), D2D1_COLOR_F{ 0.f, 0.f, 0.f, 0.35f * aB });
        if (aO <= 0.001f) return;

        // 2) .drawer-panel: transform (A2) + opacity (A3) + backdrop-filter
        dc_->SetTransform(base * scM * DrawerM());
        PushOpacity(aO);
        if (snap) {
            EnsureEffects(snap);
            DrawEffectFull(fxSatA_.Get());                            // blur(60px) saturate(1.6)
            Fill(R(0.f, 0.f, w, h), D2D1_COLOR_F{ 0.f, 0.f, 0.f, 0.35f * aB });
        }
        Fill(R(0.f, 0.f, w, h), Hex(0x0a0b0e85));                     // rgba(10,11,14,.52)
        // border 1px #ffffff14 + inset 0 1px #ffffff0f
        Fill(R(0.f, 0.f, w, 1.f), Hex(0xffffff14));
        Fill(R(0.f, 1.f, w, 2.f), Hex(0xffffff0f));
        Fill(R(0.f, h - 1.f, w, h), Hex(0xffffff14));
        Fill(R(0.f, 0.f, 1.f, h), Hex(0xffffff14));
        Fill(R(w - 1.f, 0.f, w, h), Hex(0xffffff14));

        DrawDrawerContent();
        PopOpacity();
        dc_->SetTransform(base * scM);
    }

    void DrawDrawerContent() {
        const DrawerL& L = dl_;
        if (!L.ok) return;
        auto& T = TextSys::Get();
        float ts = Clampf(tabScroll_, 0.f, L.tabMax);
        float sc = Clampf(scroll_, 0.f, L.scrollMax);

        // 拖拽把手 (A18 静态 translateX(-50%))
        FillRR(L.handle, 3.f, Hex(0xffffff47));

        // ---- tabs (横向滚动裁剪) ----
        dc_->PushAxisAlignedClip(L.tabsClip, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)L.tabR.size(); ++i) {
            D2D1_RECT_F r = ShiftX(L.tabR[(size_t)i], L.tabsClip.left - ts);
            r = ShiftY(r, L.tabsClip.top);
            if (r.right < L.tabsClip.left || r.left > L.tabsClip.right) continue;
            bool active = (i == shown_);
            float hv = (i < (int)hovTab_.size()) ? hovTab_[(size_t)i].Slow() : 0.f;
            D2D1_COLOR_F bg = active ? Hex(0xe8eaedff)
                                     : LerpC(Hex(0xffffff12), Hex(0xffffff24), hv);
            D2D1_COLOR_F bd = active ? Hex(0xe8eaedff) : Hex(0xffffff21);
            FillRR(r, RH(r) * 0.5f, bg);
            StrokeRR(r, RH(r) * 0.5f, bd);
            D2D1_COLOR_F fg = active ? Hex(0x101216ff) : Hex(0xc6ccd6ff);
            int wt = active ? 600 : 400;
            const wstring& name = groups_[(size_t)i].name;
            float nameW = T.Measure(name, 14.f, wt);
            wstring cnt = std::to_wstring(groups_[(size_t)i].idx.size());
            Txt(name, R(r.left + 18.f, r.top, r.left + 18.f + nameW + 2.f, r.bottom),
                14.f, wt, fg, 0, 1);
            Txt(cnt, R(r.left + 25.f + nameW, r.top, r.right - 18.f, r.bottom),
                11.f, 400, WithA(fg, 0.55f), 0, 1);
        }
        dc_->PopAxisAlignedClip();

        // ---- 头部 hint + 编辑按钮 ----
        Txt(L"标题栏下拉 · Esc 或 Win+D 关闭", L.hintR, 12.f, 400, Hex(0xffffff6b), 0, 1);
        DrawBtn(L.editBtn, edit_ ? L"完成" : L"编辑", hovEdit_, edit_);

        // ---- 编辑工具条 ----
        if (L.hasEditBar) {
            Fill(L.editBar, Hex(0xffffff0a));
            Fill(R(L.editBar.left, L.editBar.bottom - 1.f, L.editBar.right, L.editBar.bottom),
                 Hex(0xffffff12));
            FillRR(L.inputR, 10.f, Hex(0x00000059));
            StrokeRR(L.inputR, 10.f, Hex(0xffffff29));
            const D2D1_RECT_F tr = R(L.inputR.left + 14.f, L.inputR.top,
                                     L.inputR.right - 14.f, L.inputR.bottom);
            if (input_.empty()) {
                Txt(L"新分类名，如 影音", tr, 13.f, 400, Hex(0xffffff59), 0, 1);
            } else {
                wstring vis = T.Ellipsis(input_, RW(tr), 13.f, 400);
                Txt(vis, tr, 13.f, 400, Hex(0xe8eaedff), 0, 1);
                if (inputFocus_) {
                    float tw = T.Measure(vis, 13.f, 400);
                    float cx = tr.left + std::min(tw, std::max(0.f, RW(tr) - 1.f));
                    if (std::fmod(caretT_, 1.06) < 0.53)
                        Fill(R(cx, L.inputR.top + 7.f, cx + 1.f, L.inputR.bottom - 7.f),
                             Hex(0xe8eaedff));
                }
            }
            DrawBtn(L.addBtn, L"添加分类", hovAdd_, false);
            Txt(L"在应用上选择分类即可归类", L.editHint, 12.f, 400, Hex(0xffffff6b), 0, 1);
        }

        // ---- 正文 ----
        dc_->PushAxisAlignedClip(L.body, D2D1_ANTIALIAS_MODE_ALIASED);
        if (L.hasStatus)
            Txt(status_, ShiftY(L.statusR, -sc), 14.f, 400, Hex(0xffffff80), 1, 1);
        for (int i = 0; i < (int)L.tileR.size(); ++i) {
            float dy = -sc - 4.f * TileDy(i);
            D2D1_RECT_F r = ShiftY(L.tileR[(size_t)i], dy);
            if (r.bottom < L.body.top - 8.f || r.top > L.body.bottom + 8.f) continue;
            D2D1_RECT_F sel = (edit_ && i < (int)L.selR.size())
                              ? ShiftY(L.selR[(size_t)i], dy) : D2D1_RECT_F{ 0, 0, 0, 0 };
            DrawTile(i, r, sel);
        }
        if (L.hasEmpty)
            Txt(L"此分类暂无应用", ShiftY(L.emptyR, -sc), 14.f, 400, Hex(0xffffff73), 1, 1);
        if (L.scrollMax > 0.f) DrawVScroll(L.body, sc, L.contentH, L.scrollMax);
        dc_->PopAxisAlignedClip();

        DrawPopup();
    }

    void DrawTile(int i, const D2D1_RECT_F& r, const D2D1_RECT_F& selR) {
        const auto& items = ShownIdx();
        if (i < 0 || i >= (int)items.size()) return;
        const AppItem& a = apps_[(size_t)items[(size_t)i]];
        float tSlow = (i < (int)hovTile_.size()) ? hovTile_[(size_t)i].Slow() : 0.f;

        // A9 box-shadow 0 4px 18px rgba(0,0,0,.25) -> 0 10px 26px rgba(0,0,0,.4)
        float shOff = 4.f + 6.f * tSlow;
        float shBlur = 18.f + 8.f * tSlow;
        float shA = 0.25f + 0.15f * tSlow;
        for (int k = 3; k >= 1; --k) {
            float g = shBlur * (float)k * 0.34f;
            FillRR(R(r.left - g, r.top - g + shOff, r.right + g, r.bottom + g + shOff),
                   18.f + g, D2D1_COLOR_F{ 0.f, 0.f, 0.f, shA * (0.30f / (float)k) });
        }
        // A8 background hover
        FillRR(r, 18.f, LerpC(Hex(0xffffff0f), Hex(0xffffff21), tSlow));
        StrokeRR(r, 18.f, Hex(0xffffff1a));

        // 图标 50x50 (spec §1.1)
        D2D1_RECT_F ir = R(r.left + (RW(r) - 50.f) * 0.5f, r.top + 16.f,
                           r.left + (RW(r) + 50.f) * 0.5f, r.top + 66.f);
        int hue = (int)AppData::FallbackHue(a.id);
        bool drawn = false;
        if (a.hasIcon) {
            auto* bmp = IconBmp(iconsApp_, a.iconRgba, a.iconW, a.iconH);
            if (bmp) { DrawIconImg(bmp, a.iconW, a.iconH, ir); drawn = true; }
        }
        if (!drawn) DrawFallbackBox(ir, 14.f, FirstChar(a.name), hue, 24.f);

        // 名称: 最多 2 行 (line-clamp 2, 13px/500, lh 1.3)
        auto& T = TextSys::Get();
        float nameTop = ir.bottom + 9.f;
        int lines = 1;
        wstring nm = T.ClampLines(a.name, RW(r) - 16.f, 13.f, 500, 2, lines);
        Txt(nm, R(r.left + 8.f, nameTop, r.right - 8.f, nameTop + lines * 13.f * 1.3f),
            13.f, 500, Hex(0xdfe3e9ff), 1, 0);

        // 编辑模式分类下拉 (自绘 select)
        if (edit_ && RW(selR) > 1.f) {
            FillRR(selR, 8.f, Hex(0x0a0b0eeb));
            StrokeRR(selR, 8.f, Hex(0x5b7cfaff));
            wstring cur = Contains(cats_, a.cat) ? a.cat : L"未分类";
            Txt(cur, R(selR.left + 6.f, selR.top, selR.right - 16.f, selR.bottom),
                11.f, 400, Hex(0xe8eaedff), 0, 1);
            Txt(L"▾", R(selR.right - 15.f, selR.top, selR.right - 5.f, selR.bottom),
                10.f, 400, Hex(0xe8eaedff), 1, 1);
        }
    }

    void DrawPopup() {
        if (!popOpen_) return;
        vector<D2D1_RECT_F> items = PopupItemRects();
        if (items.empty()) return;
        D2D1_RECT_F box = R(items.front().left, items.front().top - 3.f,
                            items.front().right, items.back().bottom + 3.f);
        FillRR(box, 8.f, Hex(0x0a0b0eeb));
        StrokeRR(box, 8.f, Hex(0x5b7cfaff));
        int cur = -1;
        const auto& its = ShownIdx();
        if (popTile_ >= 0 && popTile_ < (int)its.size()) {
            const wstring& cat = apps_[(size_t)its[(size_t)popTile_]].cat;
            wstring v = Contains(cats_, cat) ? cat : L"未分类";
            for (size_t i = 0; i < popOpts_.size(); ++i)
                if (popOpts_[i] == v) { cur = (int)i; break; }
        }
        for (size_t i = 0; i < items.size() && i < popOpts_.size(); ++i) {
            if ((int)i == cur) FillRR(items[i], 4.f, Hex(0x5b7cfa59));
            if (i < hovPop_.size()) {
                float hv = hovPop_[i].Slow();
                if (hv > 0.001f)
                    FillRR(items[i], 4.f, D2D1_COLOR_F{ 0.357f, 0.486f, 0.980f, 0.35f * hv });
            }
            Txt(popOpts_[i], R(items[i].left + 8.f, items[i].top,
                               items[i].right - 8.f, items[i].bottom),
                11.f, 400, Hex(0xe8eaedff), 0, 1);
        }
    }

    // ==================== 任务面板绘制 ====================
    void DrawTasks(ID2D1Bitmap* snap, float s, const D2D1_MATRIX_3X2_F& base) {
        if (!dc_) return;
        const float w = w_, h = h_;
        float aBg = aTkB_.Value(), aO = aTkO_.Value();
        D2D1_MATRIX_3X2_F scM = D2D1::Matrix3x2F::Scale(s, s);

        // 1) .tp-backdrop: blur 0->22 saturate 1->1.2 + rgba(8,10,14,0->.62) (A10/A11 0.26s)
        dc_->SetTransform(base * scM);
        if (snap && aBg > 0.001f) {
            EnsureEffects(snap);
            if (g_uiLog) {
                D2D1_MATRIX_3X2_F t;
                dc_->GetTransform(&t);
                float sdX = 0, sdY = 0;
                snap->GetDpi(&sdX, &sdY);
                char b[240];
                sprintf_s(b, "ui/tasks-backdrop dest=(0,0,%.0f,%.0f) aBg=%.2f snapDpi=(%.1f,%.1f) xform=(%.4f %.4f %.1f %.1f)",
                    w_, h_, aBg, sdX, sdY, t._11, t._22, t._31, t._32);
                g_uiLog(b);
            }
            DrawEffectFull(fxSatB_.Get());
        }
        if (aBg > 0.001f)
            Fill(R(0.f, 0.f, w, h),
                 D2D1_COLOR_F{ 8.f / 255.f, 10.f / 255.f, 14.f / 255.f, 0.62f * aBg });
        if (aO <= 0.001f) return;

        // 2) .tp-panel: 透明背景, opacity (A12) + transform (A13)
        dc_->SetTransform(base * scM * TaskM());
        PushOpacity(aO);
        DrawTaskContent(snap);
        PopOpacity();
        dc_->SetTransform(base * scM);
    }

    void DrawTaskContent(ID2D1Bitmap* snap) {
        const TaskL& L = tl_;
        if (!L.ok) return;
        auto& T = TextSys::Get();
        float ls = Clampf(listScroll_, 0.f, L.scrollMax);

        // 五张统计卡 (spec §4.3 格式/颜色/进度条)
        StatUI su[5];
        BuildStats(su);
        for (int i = 0; i < 5; ++i) DrawStatCard(i, L, su[i], snap);

        // 区块标题: 运行中的应用 <i>count</i> (15px, ls 3px; count 12px, ml 8)
        TxtTracked(L"运行中的应用", L.sectionR, 15.f, 400, Hex(0xffffffb3), 0, 1, 3.f);
        float secW = T.Measure(L"运行中的应用", 15.f, 400) + 3.f * 5.f;
        Txt(std::to_wstring(tasks_.size()),
            R(L.sectionR.left + secW + 8.f, L.sectionR.top, L.sectionR.right, L.sectionR.bottom),
            12.f, 400, Hex(0xffffff66), 0, 1);

        // 列表 (裁剪 + 滚动阻尼)
        dc_->PushAxisAlignedClip(L.list, D2D1_ANTIALIAS_MODE_ALIASED);
        if (L.hasEmpty)
            Txt(L"暂无运行中的应用", ShiftY(L.emptyR, -ls), 14.f, 400, Hex(0xffffff73), 1, 1);
        for (int i = 0; i < (int)L.rowR.size(); ++i) {
            float dx = 6.f * RowDx(i);
            D2D1_RECT_F r = ShiftY(ShiftX(L.rowR[(size_t)i], dx), -ls);
            if (r.bottom < L.list.top - 8.f || r.top > L.list.bottom + 8.f) continue;
            D2D1_RECT_F cl = ShiftY(ShiftX(L.closeR[(size_t)i], dx), -ls);
            DrawTaskRow(i, r, cl, snap);
        }
        if (L.scrollMax > 0.f) DrawVScroll(L.list, ls, L.contentH, L.scrollMax);
        dc_->PopAxisAlignedClip();

        TxtTracked(L"按 Win 键切换 · Esc 关闭", L.hintR, 13.f, 400, Hex(0xffffff73), 1, 1, 3.f);
    }

    void DrawStatCard(int i, const TaskL& L, const StatUI& su, ID2D1Bitmap* snap) {
        D2D1_RECT_F r = L.cardR[(size_t)i];
        // 嵌套 backdrop-filter: blur(14px) (spec §5)
        if (snap && fxBlurC_) {
            dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            DrawEffectFull(fxBlurC_.Get());
            dc_->PopAxisAlignedClip();
        }
        FillRR(r, 20.f, Hex(0xffffff14));
        StrokeRR(r, 20.f, Hex(0xffffff24));
        TxtTracked(su.label, L.labelR[(size_t)i], 13.f, 400, Hex(0xffffff8c), 0, 0, 2.f);
        Txt(su.value, L.valueR[(size_t)i], 30.f, 700, su.color, 0, 0);
        if (su.hasSub) Txt(su.sub, L.subR[(size_t)i], 12.f, 400, Hex(0xffffff73), 0, 0);

        // 进度条 (A14: width 0.6s ease, bar_[i] 由 SetStats 驱动)
        D2D1_RECT_F bar = L.barR[(size_t)i];
        FillRR(bar, 3.f, Hex(0xffffff24));
        float bw = RW(bar) * Clampf(bar_[i].Value(), 0.f, 100.f) / 100.f;
        if (bw > 0.5f) FillRR(R(bar.left, bar.top, bar.left + bw, bar.bottom), 3.f, su.color);
    }

    void DrawTaskRow(int i, const D2D1_RECT_F& r, const D2D1_RECT_F& closeR, ID2D1Bitmap* snap) {
        if (i < 0 || i >= (int)tasks_.size()) return;
        const TaskItem& t = tasks_[(size_t)i];
        float tSlow = (i < (int)hovRow_.size()) ? hovRow_[(size_t)i].Slow() : 0.f;

        // 嵌套 backdrop-filter: blur(10px) (spec §5)
        if (snap && fxBlurD_) {
            dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            DrawEffectFull(fxBlurD_.Get());
            dc_->PopAxisAlignedClip();
        }
        // A15 background hover, A16 transform (translateX 已在调用前施加)
        FillRR(r, 18.f, LerpC(Hex(0xffffff1a), Hex(0xffffff2e), tSlow));
        StrokeRR(r, 18.f, Hex(0xffffff24));

        // 图标 42x42 (object-fit contain)
        D2D1_RECT_F ir = R(r.left + 20.f, r.top + 14.f, r.left + 62.f, r.top + 56.f);
        bool drawn = false;
        if (t.hasIcon) {
            auto* bmp = IconBmp(iconsTask_, t.iconRgba, t.iconW, t.iconH);
            if (bmp) { DrawIconImg(bmp, t.iconW, t.iconH, ir); drawn = true; }
        }
        if (!drawn)
            DrawFallbackBox(ir, 12.f, FirstCharUpper(t.name),
                            (int)AppData::FallbackHue(Narrow(t.id)), 20.f);

        // 文本块 (name 16/600 #f2f4f7; title 13 #ffffff80, 单行省略)
        auto& T = TextSys::Get();
        float tx = r.left + 78.f;
        float trr = closeR.left - 16.f;
        float blockH = LH(16.f) + LH(13.f);
        float ty = r.top + (RH(r) - blockH) * 0.5f;
        float avail = trr - tx;
        wstring nm = t.name;
        wstring ti = t.title.empty() ? wstring(L"—") : t.title;
        if (avail > 8.f) {
            nm = T.Ellipsis(nm, avail, 16.f, 600);
            ti = T.Ellipsis(ti, avail, 13.f, 400);
        }
        Txt(nm, R(tx, ty, trr, ty + LH(16.f)), 16.f, 600, Hex(0xf2f4f7ff), 0, 0);
        Txt(ti, R(tx, ty + LH(16.f), trr, ty + LH(16.f) + LH(13.f)), 13.f, 400, Hex(0xffffff80), 0, 0);

        // 关闭钮 .tp-close (A17)
        float hv = (i < (int)hovRowX_.size()) ? hovRowX_[(size_t)i].Slow() : 0.f;
        FillRR(closeR, RH(closeR) * 0.5f, LerpC(Hex(0xffffff1f), Hex(0xf43f5ebf), hv));
        Txt(L"✕", closeR, 16.f, 400, LerpC(Hex(0xffffffcc), Hex(0xffffffff), hv), 1, 1);
    }

    // ==================== 主绘制入口 ====================
    void Draw(ID2D1DeviceContext* dc, ID2D1Bitmap* snap, float dipW, float dipH) {
        if (!dc) return;
        if (dipW != w_ || dipH != h_) { w_ = dipW; h_ = dipH; layDirty_ = true; }
        if (w_ < 1.f || h_ < 1.f) return;

        // 设备变化 -> 丢弃设备相关资源 (brush/bitmap/effect/layer)
        ID2D1Device* dev = nullptr;
        dc->GetDevice(&dev);
        if (dev) {
            if (dev_.Get() != dev) { ResetGpu(); dev_.Reset(); dev_.Attach(dev); }
            else dev->Release();
        }

        dc_ = dc;
        if (DrawerAmt() <= 0.f && TasksAmt() <= 0.f) { dc_ = nullptr; return; }
        EnsureLayout();

        // 坐标校正: 用户单位 -> 表面像素 (s = px / dipW), 模糊半径同步换算
        float dpiX = 96.f, dpiY = 96.f;
        dc->GetDpi(&dpiX, &dpiY);
        if (dpiX <= 0.f) dpiX = 96.f;
        float s = 1.f;
        D2D1_SIZE_U px = dc->GetPixelSize();
        if (px.width > 0 && w_ > 0.f) {
            s = (float)px.width / w_;
            if (!(s >= 0.25f && s <= 4.f)) s = 1.f;
        }
        blurK_ = s * 96.f / dpiX;

        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        // z-index: drawer 10 < tasks 20 -> 先抽屉后面板
        if (DrawerAmt() > 0.f) DrawDrawer(snap, s, base);
        if (TasksAmt() > 0.f) DrawTasks(snap, s, base);

        dc->SetTransform(base);
        dc_ = nullptr;
    }



};

// ==================== OverlayUI 公有接口 ====================
OverlayUI::OverlayUI() : impl_(std::make_unique<Impl>()) { impl_->self_ = this; }
OverlayUI::~OverlayUI() = default;

void OverlayUI::OpenDrawer() { if (impl_) impl_->OpenDrawer(); }
void OverlayUI::CloseDrawer() { if (impl_) impl_->CloseDrawer(); }
void OverlayUI::ToggleTaskPanel() { if (impl_) impl_->ToggleTaskPanel(); }
void OverlayUI::CloseAll() { if (impl_) impl_->CloseAll(); }

bool OverlayUI::DrawerOpen() const {
    if (!impl_) return false;
    return impl_->state_ == Impl::Top::Drawer || impl_->DrawerAmt() > 0.001f;
}
bool OverlayUI::TasksOpen() const {
    if (!impl_) return false;
    return impl_->state_ == Impl::Top::Tasks || impl_->TasksAmt() > 0.001f;
}
bool OverlayUI::AnyVisible() const {
    if (!impl_) return false;
    return impl_->DrawerAmt() > 0.001f || impl_->TasksAmt() > 0.001f;
}
bool OverlayUI::BlocksSphere() const {
    return impl_ && impl_->state_ != Impl::Top::None;
}

bool OverlayUI::OnPointer(const PointerEvent& e) { return impl_ ? impl_->OnPointer(e) : false; }
bool OverlayUI::OnEsc() { return impl_ ? impl_->OnEsc() : false; }
bool OverlayUI::OnKey(uint32_t vk, wchar_t ch, bool down) {
    return impl_ ? impl_->OnKey(vk, ch, down) : false;
}
void OverlayUI::Tick(double dt) { if (impl_) impl_->Tick(dt); }

void OverlayUI::SetApps(const std::vector<AppItem>& apps, const std::vector<std::wstring>& cats) {
    if (impl_) impl_->SetApps(apps, cats);
}
void OverlayUI::SetTasks(const std::vector<TaskItem>& tasks) {
    if (impl_) impl_->SetTasks(tasks);
}
void OverlayUI::SetStats(const SysStats& stats) { if (impl_) impl_->SetStats(stats); }
void OverlayUI::SetDataError(bool appsUnavailable) {
    if (impl_) impl_->SetDataError(appsUnavailable);
}
bool OverlayUI::HitInteractive(float x, float y) const {
    return impl_ ? impl_->HitInteractive(x, y) : false;
}
void OverlayUI::Draw(ID2D1DeviceContext* dc, ID2D1Bitmap* sceneSnapshot, float dipW, float dipH) {
    if (impl_) impl_->Draw(dc, sceneSnapshot, dipW, dipH);
}


