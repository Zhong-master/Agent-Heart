// ============================================================
// settings.cpp — 设置窗口实现 (契约: src/settings.h)
//
// 视觉 v2 (仅重做视觉; 配置字段 / 逻辑 / 持久化格式与 v1 完全一致):
//   深色现代配色 (与原前端一致; GDI 无 alpha, 叠色已预混):
//     窗口底 #0E1116 / 主文字 #E8EAED / 次级 白55% / 描边 白14%
//     强调渐变 #5B7CFA -> #8A5CF6, 悬停 #6E8BFF
//     颜色统一走 WM_CTLCOLORSTATIC / WM_CTLCOLORBTN; 深色标题栏 (DWM 属性 20, 回退 19)
//   头部: 32px 应用图标 (LoadIconW exe 资源 id=1) + 标题 16 粗 + 副标题 13 次级
//         + 下方 3px 强调渐变横条与内容分隔; 标题栏图标走 WM_SETICON
//   卡片: 复选区 / 滑杆区两张圆角卡片 (圆角 8), 分区清晰
//   复选框: BS_OWNERDRAW 自绘 — 强调渐变方框 + 白色对勾, 悬停/按下/聚焦态
//   轨迹条: msctls_trackbar32 + 子类化自绘 — 强调色圆点滑块, 白14%轨道 + 左段强调渐变
//   按钮:   BS_OWNERDRAW 圆角 8 三态 (常态 / 悬停强调描边 / 按下);
//           主按钮「关闭」强调渐变填充白字, 次按钮「恢复默认」描边式
//   字体:   Segoe UI — 标题 16 粗 / 正文 13 / 百分比 20 粗
// ============================================================
#include "settings.h"
#include "config.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

// ---------------- 配色 (叠色已按底色预混) ----------------
constexpr COLORREF kBg       = RGB(14, 17, 22);     // #0E1116 窗口底
constexpr COLORREF kCard     = RGB(24, 26, 31);     // 卡片 (白 4% 叠底)
constexpr COLORREF kText     = RGB(232, 234, 237);  // #E8EAED 主文字
constexpr COLORREF kSecBg    = RGB(147, 148, 150);  // 次级 白55% 叠窗口底
constexpr COLORREF kSecCard  = RGB(151, 152, 154);  // 次级 白55% 叠卡片
constexpr COLORREF kHotSec   = RGB(205, 207, 211);  // 次级悬停提亮
constexpr COLORREF kStrokeBg = RGB(48, 50, 55);     // 描边 白14% 叠窗口底
constexpr COLORREF kStroke   = RGB(56, 58, 62);     // 描边 白14% 叠卡片
constexpr COLORREF kAcc1     = RGB(91, 124, 250);   // #5B7CFA 强调起
constexpr COLORREF kAcc2     = RGB(138, 92, 246);   // #8A5CF6 强调止
constexpr COLORREF kAccHot   = RGB(110, 139, 255);  // #6E8BFF 悬停
constexpr COLORREF kAccTint  = RGB(22, 28, 45);     // 强调 10% 叠底 (悬停底色)
constexpr COLORREF kDarkFill = RGB(16, 19, 24);     // 次按钮按下底
constexpr COLORREF kWhite    = RGB(255, 255, 255);

// ---------------- 布局 (固定大小, 96dpi 像素空间) ----------------
constexpr wchar_t kSettingsClass[] = L"AgentHeartSettings";

constexpr int kClientW = 440;
constexpr int kClientH = 361;
constexpr int kBarY = 64, kBarH = 3;                       // 头部强调横条
constexpr int kCardAX = 16, kCardAY = 79,  kCardAW = 408, kCardAH = 126;
constexpr int kCardBX = 16, kCardBY = 217, kCardBW = 408, kCardBH = 84;
constexpr int kChkX = 32, kChkW = 376, kChkH = 34, kChkY0 = 87, kChkStep = 38;
constexpr int kLblX = 32, kLblY = 227, kLblW = 160, kLblH = 20;
constexpr int kTrkX = 32, kTrkY = 253, kTrkW = 284, kTrkH = 30;
constexpr int kPctX = 320, kPctY = 250, kPctW = 88, kPctH = 36;
constexpr int kBtnY = 313, kBtnH = 32, kBtnW = 110;
constexpr int kResetX = 16, kCloseX = 314;                 // 314+110 = 424 = 440-16

// ---------------- 控件 id ----------------
constexpr int kIdChkAnim = 1001, kIdChkClick = 1002, kIdChkTrail = 1003;
constexpr int kIdTrack = 1004, kIdPct = 1005, kIdReset = 1006;
constexpr int kIdClose = 1007, kIdLblSize = 1008;
constexpr int kHoverMax = 16;                              // 悬停标记数组 (id-1000)

// ---------------- 窗口状态 ----------------
HWND g_hwnd = nullptr;
HWND g_chk[3]{};
HWND g_track = nullptr, g_pct = nullptr, g_lbl = nullptr;
HWND g_btnReset = nullptr, g_btnClose = nullptr;
HFONT g_fontTitle = nullptr, g_fontBody = nullptr, g_fontPct = nullptr;
HBRUSH g_bgBrush = nullptr, g_cardBrush = nullptr;
HICON g_icon = nullptr;                                    // 进程级缓存, 不销毁
bool g_hover[kHoverMax]{};
// BS_OWNERDRAW 按钮不走系统勾选态 (BM_SETCHECK/BM_GETCHECK 无效, ODS_CHECKED 不可靠),
// 勾选态由本窗口自维护镜像: 绘制/读回/切换全部走 g_chkState。
bool g_chkState[3]{};
bool g_syncing = false;                                   // SyncFromConfig 期间屏蔽回写

struct ChkRow { int id; const wchar_t* title; const wchar_t* desc; };
ChkRow g_rows[3] = {
    { kIdChkAnim,  L"启用动效", L"关闭后画面静止但仍渲染" },
    { kIdChkClick, L"点击效果", L"点击球体进入四环互换态" },
    { kIdChkTrail, L"鼠标拖尾", L"光标移动留下渐隐拖尾" },
};

bool Hover(int id) {
    const int i = id - 1000;
    return i >= 0 && i < kHoverMax && g_hover[i];
}

// 应用图标: 规格路径 (资源 id=1) -> wallpaper.rc 图标组名 -> exe 首图标 (兜底)
HICON AppIcon(HINSTANCE inst) {
    if (g_icon) return g_icon;
    g_icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!g_icon) g_icon = LoadIconW(inst, L"IDI_APPICON");
    if (!g_icon) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(inst, path, MAX_PATH);
        HICON big = nullptr;
        if (ExtractIconExW(path, 0, &big, nullptr, 1) > 0 && big) g_icon = big;
        else if (big) DestroyIcon(big);
    }
    return g_icon;
}

// 复选框切换 (自维护镜像态; 由子类 WM_LBUTTONUP / 空格键触发)
void ToggleChk(int id) {
    const int i = id - kIdChkAnim;
    if (i < 0 || i > 2) return;
    g_chkState[i] = !g_chkState[i];
    if (g_chk[i]) InvalidateRect(g_chk[i], nullptr, FALSE);
}

// ---------------- GDI 绘制工具 ----------------
COLORREF LerpColor(COLORREF a, COLORREF b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int r = GetRValue(a) + (int)((float)(GetRValue(b) - GetRValue(a)) * t + 0.5f);
    const int g = GetGValue(a) + (int)((float)(GetGValue(b) - GetGValue(a)) * t + 0.5f);
    const int bl = GetBValue(a) + (int)((float)(GetBValue(b) - GetBValue(a)) * t + 0.5f);
    return RGB(r & 255, g & 255, bl & 255);
}

COLORREF Darken(COLORREF c, float f) {
    return RGB((int)((float)GetRValue(c) * f),
               (int)((float)GetGValue(c) * f),
               (int)((float)GetBValue(c) * f));
}

// 渐变: vertical = 自上而下; 否则自左而右 (逐行/逐列填 1px, 零新增依赖)
void FillGradient(HDC dc, const RECT& rc, COLORREF a, COLORREF b, bool vertical) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (a == b) {
        HBRUSH br = CreateSolidBrush(a);
        if (br) { FillRect(dc, &rc, br); DeleteObject(br); }
        return;
    }
    if (vertical) {
        for (int i = 0; i < h; ++i) {
            const float t = (h > 1) ? (float)i / (float)(h - 1) : 0.0f;
            HBRUSH br = CreateSolidBrush(LerpColor(a, b, t));
            if (!br) continue;
            RECT row{ rc.left, rc.top + i, rc.right, rc.top + i + 1 };
            FillRect(dc, &row, br);
            DeleteObject(br);
        }
    } else {
        for (int i = 0; i < w; ++i) {
            const float t = (w > 1) ? (float)i / (float)(w - 1) : 0.0f;
            HBRUSH br = CreateSolidBrush(LerpColor(a, b, t));
            if (!br) continue;
            RECT col{ rc.left + i, rc.top, rc.left + i + 1, rc.bottom };
            FillRect(dc, &col, br);
            DeleteObject(br);
        }
    }
}

// 圆角裁剪填充 (半径 = radius, 先裁剪后填充, 再恢复 DC)
void FillRounded(HDC dc, RECT rc, COLORREF a, COLORREF b, bool vertical, int radius) {
    if (rc.right <= rc.left || rc.bottom <= rc.top || radius <= 0) return;
    const int save = SaveDC(dc);
    if (BeginPath(dc)) {
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
        if (EndPath(dc)) SelectClipPath(dc, RGN_COPY);     // 圆角裁剪 (失败则退化直角)
    }
    FillGradient(dc, rc, a, b, vertical);
    RestoreDC(dc, save);
}

// 圆角描边 (1px 时对齐像素: 右/下各收 1)
void StrokeRounded(HDC dc, RECT rc, COLORREF c, int width, int radius) {
    if (rc.right <= rc.left || rc.bottom <= rc.top || radius <= 0 || width <= 0) return;
    --rc.right;
    --rc.bottom;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    HPEN pen = CreatePen(PS_SOLID, width, c);
    if (!pen) return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

HFONT MakeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void DrawTextIn(HDC dc, HFONT font, COLORREF color, const wchar_t* s, RECT rc, UINT extra) {
    if (!dc || !font || !s) return;
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, &rc, extra | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void DrawTrackBar(HWND h, HDC dc);   // 前置声明 (子类过程使用)

// ---------------- 轨迹条自绘 (强调圆点滑块 + 半透明浅色轨道) ----------------
void DrawTrackBar(HWND h, HDC dc) {
    RECT rc{};
    GetClientRect(h, &rc);
    if (g_cardBrush) FillRect(dc, &rc, g_cardBrush);       // 卡片底 (与所在卡片同色)

    RECT ch{};
    SendMessageW(h, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&ch));
    const int rmin = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0);
    const int rmax = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
    const int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
    const int span = (rmax > rmin) ? (rmax - rmin) : 1;
    const int trackW = ch.right - ch.left;
    const int cy = (ch.top + ch.bottom) / 2;
    if (trackW <= 2 || ch.bottom <= ch.top) return;

    float frac = (float)(pos - rmin) / (float)span;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // 轨道: 白 14% 半透明浅色, 高 6 圆角 3
    RECT track{ ch.left, cy - 3, ch.right, cy + 3 };
    FillRounded(dc, track, kStroke, kStroke, false, 3);

    // 左段: 强调渐变 (右端被滑块圆点覆盖)
    const int thumbCx = ch.left + (int)(frac * (float)trackW);
    if (thumbCx - track.left > 2) {
        RECT filled{ track.left, track.top, thumbCx, track.bottom };
        FillRounded(dc, filled, kAcc1, kAcc2, false, 3);
    }

    // 滑块: 16px 强调圆点 (悬停提亮), 卡片色描边与轨道分离
    const bool hv = Hover(kIdTrack);
    RECT th{ thumbCx - 8, cy - 8, thumbCx + 8, cy + 8 };
    FillRounded(dc, th, hv ? kAccHot : kAcc1, hv ? kAccHot : kAcc2, true, 8);
    StrokeRounded(dc, th, g_cardBrush ? kCard : kBg, 2, 8);
}

// ---------------- 控件子类: 悬停跟踪 + 轨迹条自绘 ----------------
LRESULT CALLBACK CtlSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    const int idx = (int)id - 1000;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (idx >= 0 && idx < kHoverMax && !g_hover[idx]) {
            g_hover[idx] = true;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (idx >= 0 && idx < kHoverMax && g_hover[idx]) {
            g_hover[idx] = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONUP: {                                  // 复选框点击: 自维护勾选态
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);  // 先让按钮收起按下态
        if (id >= (UINT_PTR)kIdChkAnim && id <= (UINT_PTR)kIdChkTrail) {
            ToggleChk((int)id);
            if (GetParent(h))
                SendMessageW(GetParent(h), WM_COMMAND,
                             MAKEWPARAM((int)id, BN_CLICKED), reinterpret_cast<LPARAM>(h));
        }
        return r;
    }
    case WM_KEYDOWN:                                       // 复选框空格/回车: 同点击
        if (id >= (UINT_PTR)kIdChkAnim && id <= (UINT_PTR)kIdChkTrail &&
            (wp == VK_SPACE || wp == VK_RETURN) &&
            (static_cast<LPARAM>(lp) & ((LPARAM)1 << 30)) == 0) {
            const LRESULT r = DefSubclassProc(h, msg, wp, lp);
            ToggleChk((int)id);
            if (GetParent(h))
                SendMessageW(GetParent(h), WM_COMMAND,
                             MAKEWPARAM((int)id, BN_CLICKED), reinterpret_cast<LPARAM>(h));
            return r;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (id == (UINT_PTR)kIdTrack) {                    // 轨迹条全自绘
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            DrawTrackBar(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

HWND MakeCtrl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
              int x, int y, int w, int h, int id, HFONT font, bool subclass) {
    HWND ctrl = CreateWindowExW(0, cls, text, style, x, y, w, h, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    if (!ctrl) return nullptr;
    if (font) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (subclass) SetWindowSubclass(ctrl, CtlSubclass, static_cast<UINT_PTR>(id), 0);
    return ctrl;
}

// ---------------- 控件 <-> 配置 (与 v1 相同逻辑) ----------------
void UpdatePctLabel() {
    if (!g_pct || !g_track) return;
    const int pos = static_cast<int>(SendMessageW(g_track, TBM_GETPOS, 0, 0));
    wchar_t buf[32] = L"";
    swprintf_s(buf, _countof(buf), L"%d%%", pos);
    SetWindowTextW(g_pct, buf);
}

void SyncFromConfig() {                                    // 配置 -> 控件
    g_syncing = true;                                      // 屏蔽 TBM_SETPOS 触发的回写
    const AppConfig& c = ConfigGet();
    g_chkState[0] = c.animationsEnabled;
    g_chkState[1] = c.clickEffectEnabled;
    g_chkState[2] = c.mouseTrailEnabled;
    for (int i = 0; i < 3; ++i) {
        if (!g_chk[i]) continue;
        SendMessageW(g_chk[i], BM_SETCHECK,
                     g_chkState[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        InvalidateRect(g_chk[i], nullptr, FALSE);          // owner-draw 不随 BM_SETCHECK 重绘
    }
    if (g_track) {
        int pos = static_cast<int>(c.sphereScale * 100.0f + 0.5f);
        if (pos < 50) pos = 50;
        if (pos > 150) pos = 150;
        SendMessageW(g_track, TBM_SETPOS, TRUE, static_cast<LPARAM>(pos));
    }
    UpdatePctLabel();
    g_syncing = false;
}

void ApplyFromControls() {                                 // 控件 -> 配置 (改动即保存)
    if (!g_chk[0] || !g_chk[1] || !g_chk[2] || !g_track) return;
    AppConfig c = ConfigGet();
    c.animationsEnabled = g_chkState[0];                   // 读自维护镜像 (非 BM_GETCHECK)
    c.clickEffectEnabled = g_chkState[1];
    c.mouseTrailEnabled = g_chkState[2];
    const int pos = static_cast<int>(SendMessageW(g_track, TBM_GETPOS, 0, 0));
    c.sphereScale = static_cast<float>(pos) / 100.0f;      // 50~150 -> 0.5~1.5
    ConfigSet(c);                                           // 立即落盘
    UpdatePctLabel();
}

// ---------------- 自绘: 复选框 (强调对勾 + 三态) ----------------
void DrawCheckboxItem(const DRAWITEMSTRUCT* ds, int row) {
    HDC dc = ds->hDC;
    if (!dc || row < 0 || row > 2) return;
    const RECT& rc = ds->rcItem;
    if (g_cardBrush) FillRect(dc, &rc, g_cardBrush);        // 卡片底
    const bool checked = g_chkState[row];                   // 自维护镜像态 (不看 ODS_CHECKED)
    const bool pressed = (ds->itemState & ODS_SELECTED) != 0;
    const bool hover = Hover(ds->CtlID);

    // 18x18 方框 (对齐标题行)
    RECT box{ rc.left + 1, rc.top + 1, rc.left + 19, rc.top + 19 };
    if (checked) {
        COLORREF a = kAcc1, b = kAcc2;
        if (pressed) { a = Darken(a, 0.85f); b = Darken(b, 0.85f); }
        FillRounded(dc, box, a, b, true, 4);
    } else if (hover || pressed) {
        FillRounded(dc, box, kAccTint, kAccTint, true, 4);
    }
    const COLORREF stroke = checked ? (hover ? kAccHot : kAcc2)
                                    : (hover ? kAccHot : kStroke);
    StrokeRounded(dc, box, stroke, 1, 4);

    if (checked) {                                          // 白色对勾
        HPEN pen = CreatePen(PS_SOLID | PS_ENDCAP_FLAT | PS_JOIN_ROUND, 2, kWhite);
        if (pen) {
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            MoveToEx(dc, rc.left + 5, rc.top + 9, nullptr);
            LineTo(dc, rc.left + 9, rc.top + 13);
            LineTo(dc, rc.left + 15, rc.top + 5);
            SelectObject(dc, oldBr);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
    }

    // 标题 (正文13 主色) + 描述 (正文13 次级)
    const ChkRow& r = g_rows[row];
    DrawTextIn(dc, g_fontBody, hover ? kWhite : kText, r.title,
               RECT{ rc.left + 30, rc.top, rc.right - 6, rc.top + 18 }, DT_LEFT);
    DrawTextIn(dc, g_fontBody, hover ? kHotSec : kSecCard, r.desc,
               RECT{ rc.left + 30, rc.top + 18, rc.right - 6, rc.top + 34 }, DT_LEFT);

    if (ds->itemState & ODS_FOCUS) {                        // 键盘聚焦: 强调色环
        RECT f = rc;
        InflateRect(&f, -2, -2);
        StrokeRounded(dc, f, kAccHot, 1, 8);
    }
}

// ---------------- 自绘: 按钮 (圆角8 三态) ----------------
void DrawButtonItem(const DRAWITEMSTRUCT* ds, bool primary) {
    HDC dc = ds->hDC;
    if (!dc) return;
    const RECT rc = ds->rcItem;
    const bool hover = Hover(ds->CtlID);
    const bool pressed = (ds->itemState & ODS_SELECTED) != 0;
    if (g_bgBrush) FillRect(dc, &rc, g_bgBrush);            // 圆角外露出窗口底

    if (primary) {                                          // 主按钮: 强调渐变填充白字
        COLORREF a = kAcc1, b = kAcc2;
        if (hover && !pressed) a = kAccHot;
        if (pressed) { a = Darken(kAcc1, 0.78f); b = Darken(kAcc2, 0.78f); }
        FillRounded(dc, rc, a, b, true, 8);
        const COLORREF st = pressed ? Darken(kAccHot, 0.85f) : (hover ? kAccHot : kStrokeBg);
        StrokeRounded(dc, rc, st, 1, 8);
    } else {                                                // 次按钮: 描边式三态
        const COLORREF fill = pressed ? kDarkFill : (hover ? kAccTint : kCard);
        FillRounded(dc, rc, fill, fill, true, 8);
        StrokeRounded(dc, rc, hover ? kAccHot : kStrokeBg, 1, 8);
    }

    const wchar_t* label = primary ? L"关闭" : L"恢复默认";
    const COLORREF tc = primary ? kWhite : ((hover || pressed) ? kWhite : kText);
    DrawTextIn(dc, g_fontBody, tc, label, rc,
               primary ? DT_CENTER : DT_CENTER);

    if (ds->itemState & ODS_FOCUS) {                        // 聚焦环
        RECT f = rc;
        InflateRect(&f, -3, -3);
        StrokeRounded(dc, f, primary ? kWhite : kAccHot, 1, 8);
    }
}

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                                           // 背景自绘, 防闪烁

    case WM_CREATE: {
        g_fontTitle = MakeFont(16, FW_BOLD);
        g_fontBody = MakeFont(13, FW_NORMAL);
        g_fontPct = MakeFont(20, FW_BOLD);
        g_bgBrush = CreateSolidBrush(kBg);
        g_cardBrush = CreateSolidBrush(kCard);
        g_icon = AppIcon(GetModuleHandleW(nullptr));       // 资源 id=1 (三级回退)
        if (g_icon) {
            SendMessageW(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
            SendMessageW(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));
        }

        const DWORD chkStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               BS_OWNERDRAW | BS_AUTOCHECKBOX;
        for (int i = 0; i < 3; ++i)
            g_chk[i] = MakeCtrl(h, L"BUTTON", g_rows[i].title, chkStyle,
                                kChkX, kChkY0 + i * kChkStep, kChkW, kChkH,
                                g_rows[i].id, g_fontBody, true);

        g_lbl = MakeCtrl(h, L"STATIC", L"球形大小", WS_CHILD | WS_VISIBLE,
                         kLblX, kLblY, kLblW, kLblH, kIdLblSize, g_fontBody, false);

        g_track = MakeCtrl(h, TRACKBAR_CLASSW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                           TBS_AUTOTICKS | TBS_NOTICKS,
                           kTrkX, kTrkY, kTrkW, kTrkH, kIdTrack, nullptr, true);
        if (g_track) {
            SendMessageW(g_track, TBM_SETRANGE, TRUE, MAKELPARAM(50, 150));   // 50%~150%
            SendMessageW(g_track, TBM_SETTICFREQ, 10, 0);
            SendMessageW(g_track, TBM_SETPAGESIZE, 0, 5);
        }

        g_pct = MakeCtrl(h, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                         kPctX, kPctY, kPctW, kPctH, kIdPct, g_fontPct, false);

        g_btnReset = MakeCtrl(h, L"BUTTON", L"恢复默认",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                              kResetX, kBtnY, kBtnW, kBtnH, kIdReset, g_fontBody, true);
        g_btnClose = MakeCtrl(h, L"BUTTON", L"关闭",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                              kCloseX, kBtnY, kBtnW, kBtnH, kIdClose, g_fontBody, true);

        SyncFromConfig();                                   // 打开时 配置 -> 控件
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (dc) {
            RECT rc{};
            GetClientRect(h, &rc);
            if (g_bgBrush) FillRect(dc, &rc, g_bgBrush);

            // 两张圆角卡片 (分区)
            const RECT cardA{ kCardAX, kCardAY, kCardAX + kCardAW, kCardAY + kCardAH };
            const RECT cardB{ kCardBX, kCardBY, kCardBX + kCardBW, kCardBY + kCardBH };
            FillRounded(dc, cardA, kCard, kCard, true, 8);
            StrokeRounded(dc, cardA, kStrokeBg, 1, 8);
            FillRounded(dc, cardB, kCard, kCard, true, 8);
            StrokeRounded(dc, cardB, kStrokeBg, 1, 8);

            // 头部: 32px 图标 + 标题 16 粗 + 副标题 13 次级
            const RECT iconRc{ 20, 16, 52, 48 };
            if (g_icon) {
                DrawIconEx(dc, iconRc.left, iconRc.top, g_icon, 32, 32, 0, nullptr, DI_NORMAL);
            } else {                                        // 资源缺失: 强调渐变占位
                FillRounded(dc, iconRc, kAcc1, kAcc2, true, 8);
                DrawTextIn(dc, g_fontTitle, kWhite, L"A", iconRc, DT_CENTER);
            }
            DrawTextIn(dc, g_fontTitle, kText, L"Agent-Heart 设置",
                       RECT{ 64, 12, kClientW - 16, 34 }, DT_LEFT);
            DrawTextIn(dc, g_fontBody, kSecBg, L"壁纸显示与交互偏好",
                       RECT{ 64, 34, kClientW - 16, 56 }, DT_LEFT);

            // 3px 强调渐变横条 (与内容分隔)
            const RECT bar{ 0, kBarY, kClientW, kBarY + kBarH };
            FillGradient(dc, bar, kAcc1, kAcc2, false);
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {                               // 静态文本统一深底浅字
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND ctl = reinterpret_cast<HWND>(lp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (ctl == g_pct) ? kText : kSecCard);
        return reinterpret_cast<LRESULT>(g_cardBrush ? g_cardBrush : g_bgBrush);
    }

    case WM_CTLCOLORBTN: {                                  // 按钮底色 (自绘前的兜底)
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(g_bgBrush ? g_bgBrush : g_cardBrush);
    }

    case WM_DRAWITEM: {
        auto* ds = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!ds || ds->CtlType != ODT_BUTTON) break;
        switch (ds->CtlID) {
        case kIdChkAnim:  DrawCheckboxItem(ds, 0); return TRUE;
        case kIdChkClick: DrawCheckboxItem(ds, 1); return TRUE;
        case kIdChkTrail: DrawCheckboxItem(ds, 2); return TRUE;
        case kIdReset:    DrawButtonItem(ds, false); return TRUE;
        case kIdClose:    DrawButtonItem(ds, true);  return TRUE;
        default: break;
        }
        break;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == kIdChkAnim || id == kIdChkClick || id == kIdChkTrail) {
            if (HIWORD(wp) == BN_CLICKED) ApplyFromControls();   // 复选: 改动即存
        } else if (id == kIdReset) {
            ConfigResetDefaults();                           // 恢复默认 (含落盘)
            SyncFromConfig();                                // 控件回显
        } else if (id == kIdClose) {
            DestroyWindow(h);
        }
        return 0;
    }

    case WM_HSCROLL:                                        // 轨迹条拖动/点击
        if (!g_syncing && reinterpret_cast<HWND>(lp) == g_track) ApplyFromControls();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) SyncFromConfig();     // 双向同步: 回读配置
        return 0;

    case WM_GETMINMAXINFO: {                                // 锁死固定大小
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        RECT rc{ 0, 0, kClientW, kClientH };
        const DWORD st = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE));
        const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE));
        AdjustWindowRectEx(&rc, st, FALSE, ex);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        mmi->ptMaxTrackSize.x = mmi->ptMinTrackSize.x;
        mmi->ptMaxTrackSize.y = mmi->ptMinTrackSize.y;
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_hwnd = nullptr;
        g_chk[0] = g_chk[1] = g_chk[2] = nullptr;
        g_track = g_pct = g_lbl = g_btnReset = g_btnClose = nullptr;
        for (int i = 0; i < kHoverMax; ++i) g_hover[i] = false;
        if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
        if (g_fontBody)  { DeleteObject(g_fontBody);  g_fontBody = nullptr; }
        if (g_fontPct)   { DeleteObject(g_fontPct);   g_fontPct = nullptr; }
        if (g_bgBrush)   { DeleteObject(g_bgBrush);   g_bgBrush = nullptr; }
        if (g_cardBrush) { DeleteObject(g_cardBrush); g_cardBrush = nullptr; }
        return 0;                                           // g_icon 进程级缓存, 不销毁
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowSettingsWindow() {
    // 单实例: 已开则置前
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;                          // msctls_trackbar32 类组
    InitCommonControlsEx(&icc);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                             // 背景自绘 (WM_PAINT)
    wc.hIcon = AppIcon(inst);                               // 标题栏图标 (资源 id=1 回退链)
    wc.lpszClassName = kSettingsClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;   // 固定, 不可缩放
    RECT rc{ 0, 0, kClientW, kClientH };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int w = rc.right - rc.left;
    const int hh = rc.bottom - rc.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - hh) / 2;

    g_hwnd = CreateWindowExW(0, kSettingsClass, L"Agent-Heart 设置", style,
                             x, y, w, hh, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return;

    BOOL dark = TRUE;                                       // 深色标题栏 (旧版回退 19)
    if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
}
