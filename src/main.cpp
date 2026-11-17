// ============================================================
// NativeWallpaper — 原生 Windows 壁纸宿主 (Stage 1)
//
// 形态: 把全屏窗口挂到 explorer 的 WorkerW 桌面层 (图标之下),
//       常驻后台、不进任务栏、不抢焦点; 纯 Win32 + D2D/Composition,
//       零 HTML / 零 WebView / 零第三方。
//
// 模式:
//   Agent-Heart.exe                   # 壁纸模式 (默认, 挂桌面层)
//   Agent-Heart.exe --windowed        # 普通窗口 (开发验证)
//   Agent-Heart.exe --probe           # 壁纸 + 输入/窗口结构探针日志
//
// 退出: 托盘图标右键 -> 退出
// 日志: exe 同目录 Agent-Heart.log (随可执行文件名变化)
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <oleacc.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi.h>
#include <uiautomation.h>

#include "sphere_scene.h"
#include "app_data.h"
#include "ui_overlay.h"
#include "config.h"
#include "settings.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")

// CoreMessaging.dll: Win32 线程创建 DispatcherQueue (DesktopWindowTarget 硬前提)。
// 自行声明以避开 um\DispatcherQueue.h 与 cppwinrt 头的路径冲突。
struct DispatcherQueueOptions {
    unsigned long dwSize;
    int threadType;      // 2 = DQTYPE_THREAD_CURRENT
    int apartmentType;
};
extern "C" HRESULT WINAPI CreateDispatcherQueueController(
    DispatcherQueueOptions options, void** dispatcherQueueController);

using namespace winrt;
using namespace Windows::UI::Composition;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;

namespace {

constexpr wchar_t kClassName[] = L"NativeWallpaperWnd";
constexpr wchar_t kSingleInstanceMutex[] = L"NativeWallpaper_SingleInstance";
constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kTrayUid = 1;
constexpr UINT kIdExit = 2001;
constexpr UINT kIdReload = 2002;
constexpr UINT kIdSettings = 2003;   // 托盘 设置(S)

std::wstring g_logPath;
bool g_probe = false;
bool g_fullscreen = true;   // wallpaper mode

class WallApp;              // 低级钩子回调入口
static WallApp* g_app = nullptr;

// 自动化验收参数 (--shot/--open/--interact)
std::wstring g_shotPath;          // 非空 = 到帧 kShotFrame 时把合成表面存成 PNG
std::string g_open;               // "drawer" | "tasks" | ""
std::string g_interact;           // "hover" | "click" | "drag" | ""
bool g_shotDone = false;
std::wstring g_selfShotPath;    // --selfshot: 进程内 PrintWindow 抓自身窗口合成内容
bool g_selfShotDone = false;
std::wstring g_snapPath;        // --snapshot: 导出毛玻璃输入 (场景快照) 本体
bool g_snapDone = false;
bool g_selfTest = false;        // --selftest-input: 进程内注入输入, 自证钩子链路
// 用户裁定 (2026-11-17): 只要壁纸+动效 —— 抽屉/任务界面两个入口关闭 (代码保留, 改 true 即恢复)
constexpr bool kOverlaysEnabled = false;
unsigned g_tbpRr = 0;           // timeBeginPeriod(1) 返回值
int g_llSeen = 0;               // 钩子收到的鼠标事件累计 (自测判据)
POINT g_savedCursor{0, 0};
constexpr int kShotFrame = 90;    // 1.5s @60fps: 打开/形变动效已过半
winrt::com_ptr<ID2D1Bitmap1> g_shotBmp;   // (v1 遗留, 不再使用)
winrt::com_ptr<ID3D11Texture2D> g_shotStage;   // v2: D3D11 staging 回读
winrt::com_ptr<ID3D11DeviceContext> g_shotCtx;

// UIA (桌面图标是虚拟列表视图, MSAA 子项枚举不可靠; ElementFromPoint 与生产命中同源)
IUIAutomation* g_uga = nullptr;
IUIAutomation* Uia() {
    if (!g_uga) {
        const GUID kCUia = { 0xff48dba4, 0x60ef, 0x4201,
            { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };   // CLSID_CUIAutomation
        CoCreateInstance(kCUia, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&g_uga));
    }
    return g_uga;
}

void Log(const std::string& msg) {
    OutputDebugStringA(("[wp] " + msg + "\n").c_str());
    if (g_logPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a") == 0 && f) {
        fputs(msg.c_str(), f);
        fputc('\n', f);
        fclose(f);
    }
}

void LogEx(const char* what, winrt::hresult_error const& e) {
    char buf[16];
    sprintf_s(buf, "%08X", static_cast<unsigned>(e.code()));
    Log(std::string(what) + " FAILED: hr=0x" + buf + " " + winrt::to_string(e.message()));
}

std::string Narrow(std::wstring const& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ClassNameOf(HWND h) {
    wchar_t buf[128] = L"";
    if (h) GetClassNameW(h, buf, 128);
    char hex[32];
    sprintf_s(hex, " [%p]", (void*)h);
    return Narrow(buf) + hex;
}

// 窗口所属进程名 (探针诊断用)
std::string ProcNameOf(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::string name = "pid" + std::to_string(pid);
    if (pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (p) {
            wchar_t buf[MAX_PATH] = L"";
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, buf, &sz)) {
                std::wstring s(buf);
                auto pos = s.find_last_of(L'\\');
                name = Narrow(pos == std::wstring::npos ? s : s.substr(pos + 1)) +
                    ":" + std::to_string(pid);
            }
            CloseHandle(p);
        }
    }
    return name;
}

// ---------------- WorkerW 解析 ----------------

BOOL CALLBACK FindDefViewEnum(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        *reinterpret_cast<HWND*>(lp) = top;
        return FALSE;
    }
    return TRUE;
}

// 顶层窗口结构快照 (探针): Z 序从顶到底, 含隐藏窗口, 标注 DefView 归属与进程
void LogWindowTree() {
    if (!g_probe) return;
    Log("--- top-level z-order: idx V/H class[hex] proc [DefView] |children ---");
    HWND w = GetTopWindow(nullptr);
    int i = 0;
    while (w && i < 120) {
        std::string line = "  #" + std::to_string(i) + (IsWindowVisible(w) ? " V " : " H ");
        line += ClassNameOf(w);
        line += " " + ProcNameOf(w);
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) line += " [DefView+]";
        HWND c = GetWindow(w, GW_CHILD);
        int ci = 0;
        while (c && ci < 4) {
            line += " |" + ClassNameOf(c);
            c = GetWindow(c, GW_HWNDNEXT);
            ++ci;
        }
        Log(line);
        w = GetWindow(w, GW_HWNDNEXT);
        ++i;
    }
    Log("--- tree end (" + std::to_string(i) + " walked) ---");
}


// ---------------- 应用 ----------------

class WallApp {
public:
    bool Initialize(HINSTANCE inst);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Layout();
    void SetRootVisual();
    bool AttachToDesktop();
    void ProbeInput();
    void SetupTray();
    void RemoveTray();
    void ShowTrayMenu(POINT pt);
    void OnFrameTick();
    void NotePointer(UINT kind, POINT pt, WPARAM extra);  // Stage1: 只记日志
    HWND hwndForDiag() const { return hwnd_; }

    // ---- Stage2/3: 输入中枢 (低级鼠标/键盘钩子) ----
    static LRESULT CALLBACK LLMouseProc(int nCode, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK LLKeyProc(int nCode, WPARAM wp, LPARAM lp);
    void InstallHooks();
    void RemoveHooks();
    bool HandleLLMouse(WPARAM msg, MSLLHOOKSTRUCT* info);   // true = 吞掉该事件
    bool HandleLLKey(WPARAM msg, KBDLLHOOKSTRUCT* info);    // true = 吞掉该按键
    void RouteMove(float x, float y, bool down);
    void RouteDown(float x, float y);
    void RouteUp(float x, float y);
    void RouteWheel(float x, float y, int delta);
    bool PointOverDesktopIcon(POINT pt, bool* checkFailed) const;  // UIA accHitTest
    bool IsDesktopForeground() const;
    void EnsureAppsLoaded();
    void PollTaskData(double dt);
    float ToDipX(int screenX) const;   // 屏幕像素 -> 客户区 DIP
    float ToDipY(int screenY) const;

    HWND hwnd_{nullptr};
    void* dqController_{nullptr};
    ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget* target_{nullptr};
    Compositor compositor_{nullptr};
    SpriteVisual root_{nullptr};
    SphereScene sphere_;
    Windows::System::DispatcherQueueTimer frameTimer_{nullptr};
    std::chrono::steady_clock::time_point lastTick_{};
    bool trayAdded_{false};
    UINT taskbarCreatedMsg_{0};
    bool attached_{false};

    // 输入中枢状态
    OverlayUI ui_;
    AppData data_;
    float dpiScale_{1.0f};
    HHOOK llMouse_{nullptr}, llKey_{nullptr};
    bool gestureActive_{false};     // 已路由的左键手势 (down 已发, 待 up 闭合)
    bool leftSwallowed_{false};     // 该手势的 down 被我们吞掉
    bool winDown_{false}, winSwallow_{false}, winComboInjected_{false}, winOtherKey_{false};
    bool vkSwallowed_[256]{};
    bool appsLoaded_{false};
    bool tasksPrimed_{false};
    double taskPoll_{0};

    // 探针节流
    int probeEvents_ = 0;
    int lastLogX_ = -99999, lastLogY_ = -99999;
};

// ---- 布局: 全屏按虚拟屏, 窗口模式按 client ----
void WallApp::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float pxW = (float)(rc.right - rc.left);
    float pxH = (float)(rc.bottom - rc.top);
    if (pxW <= 0 || pxH <= 0) return;
    float dpi = (float)GetDpiForWindow(hwnd_);
    if (dpi <= 0) dpi = 96.0f;
    // 关键修正 (用户实测 B: 右/下露原壁纸): DesktopWindowTarget 的合成以 96dpi 工作
    // (插值 DC GetDpi 实测=96), 即 DIP == 物理像素 —— 不能再除以窗口 DPI 缩放,
    // 否则 root/球体视觉体只有左上 80%, 右 624px + 下 416px 露出原壁纸。
    // 统一按像素空间: root/球体视觉体/指针坐标全部 = 物理像素。
    dpiScale_ = 1.0f;
    if (root_) root_.Size({ pxW, pxH });
    {
        static bool logged = false;
        if (!logged && root_) {
            logged = true;
            char lb[200];
            sprintf_s(lb, "layout: clientPx=%.0fx%.0f (pixel space, windowDpi=%.0f ignored)",
                pxW, pxH, dpi);
            Log(lb);
        }
    }
    sphere_.SetDpiScale(1.0f);
    sphere_.Resize(pxW, pxH);
}

void WallApp::SetRootVisual() {
    using TargetAbi = winrt::impl::abi<winrt::Windows::UI::Composition::Desktop::IDesktopWindowTarget>::type;
    using CompTargetAbi = winrt::impl::abi<winrt::Windows::UI::Composition::ICompositionTarget>::type;
    CompTargetAbi* ct = nullptr;
    check_hresult(reinterpret_cast<TargetAbi*>(target_)->QueryInterface(
        winrt::guid_of<winrt::Windows::UI::Composition::ICompositionTarget>(),
        reinterpret_cast<void**>(&ct)));
    check_hresult(ct->put_Root(*reinterpret_cast<void**>(&root_)));
    ct->Release();
}

bool WallApp::AttachToDesktop() {
    // 1) 让 explorer 生成/整理壁纸 WorkerW
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        DWORD_PTR r = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 2000, &r);
        Log("Progman found, sent 0x052C(0) result=" + std::to_string((unsigned long long)r));
    } else {
        Log("Progman NOT found (explorer not running?)");
    }

    // 2) 找图标层 (含 SHELLDLL_DefView 的顶层窗口)
    HWND icons = nullptr;
    EnumWindows(FindDefViewEnum, reinterpret_cast<LPARAM>(&icons));
    if (icons) Log("icons window = " + ClassNameOf(icons) + " " + ProcNameOf(icons));
    else Log("no SHELLDLL_DefView found");

    // 3) 策略选择
    HWND host = nullptr;
    std::string strategy;
    if (icons) {                                   // A: 图标层下方的顶层 WorkerW
        host = FindWindowExW(nullptr, icons, L"WorkerW", nullptr);
        if (host) strategy = "A: WorkerW below icons";
    }
    if (!host && icons && progman && icons == progman) {  // B: DefView 在 Progman -> 挂进 Progman 底部
        host = progman;
        strategy = "B: child of Progman bottom";
    }
    if (!host && progman) {                        // C: 0x052C wParam=0xD 重试 + 重扫
        DWORD_PTR r = 0;
        SendMessageTimeoutW(progman, 0x052C, 0xD, 0, SMTO_NORMAL, 2000, &r);
        Log("sent 0x052C(0xD) result=" + std::to_string((unsigned long long)r));
        EnumWindows(FindDefViewEnum, reinterpret_cast<LPARAM>(&icons));
        if (icons) {
            host = FindWindowExW(nullptr, icons, L"WorkerW", nullptr);
            if (host) strategy = "C: WorkerW below icons after 0xD";
        }
        if (!host) {                               // C2: 第一个无 DefView 的 WorkerW
            HWND w = nullptr;
            while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr))) {
                if (!FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
                    host = w;
                    strategy = "C2: first DefView-less WorkerW";
                    break;
                }
            }
        }
        if (!host) {                               // D: Progman 兜底
            host = progman;
            strategy = "D: Progman fallback";
        }
    }
    if (!host) {
        Log("no wallpaper host found -> wallpaper attach aborted");
        return false;
    }
    Log("host strategy: " + strategy + " -> " + ClassNameOf(host) + " " + ProcNameOf(host));

    // 4) popup -> child 样式转换 (SetParent 对 WS_POPUP 会 ERROR_INVALID_PARAMETER)
    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    SetWindowLongPtrW(hwnd_, GWL_STYLE,
        (style & ~WS_POPUP & ~WS_THICKFRAME & ~WS_CAPTION & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX) | WS_CHILD);

    SetLastError(0);
    SetParent(hwnd_, host);
    DWORD err = GetLastError();
    HWND now = GetParent(hwnd_);
    Log("SetParent verify: GetParent=" + ClassNameOf(now) +
        " expected=" + ClassNameOf(host) + " err=" + std::to_string(err));
    if (now != host) {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);   // 回滚
        return false;
    }

    // 5) 铺满宿主客户区并压到最底 (同父时保证在 DefView 之下)
    RECT rc{};
    GetClientRect(host, &rc);
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // 关键 z 序 (实测): Progman 内存在全屏 vis=1 的 WorkerW (explorer 壁纸层),
    // HWND_BOTTOM 会让我们压在它下面 -> 原壁纸盖住我们 -> 用户看不到变化。
    // 提到 SHELLDLL_DefView 正下方: 图标仍最上, 我们盖住壁纸层。 (hwndInsertAfter=DefView → 我们在其下)
    {
        HWND defView = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) {
            SetWindowPos(hwnd_, defView, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            Log("z-order: placed just below DefView (above fullscreen WorkerW)");
        } else {
            Log("z-order: DefView not child of host; keep HWND_BOTTOM");
        }
    }
    attached_ = true;
    Log("attached, client=" + std::to_string(rc.right - rc.left) + "x" +
        std::to_string(rc.bottom - rc.top));
    Layout();
    ProbeInput();
    return true;
}

// 可见性诊断用: Progman 子窗枚举 (静态 CALLBACK, 避免 lambda->函数指针转换在部分工具链下崩溃)
struct VisCtx { int i; };
BOOL CALLBACK VisChildEnum(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<VisCtx*>(lp);
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    RECT rc{};
    GetWindowRect(h, &rc);
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    bool self = (wpid == GetCurrentProcessId());   // 用 PID 识别自身, 绕开私有成员访问
    char b[256];
    sprintf_s(b, "visibility: child#%d %s%s vis=%d rect=(%ld,%ld) %ldx%ld",
        c->i, Narrow(std::wstring(cls)).c_str(), self ? " <== US" : "",
        IsWindowVisible(h) ? 1 : 0,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    Log(b);
    ++c->i;
    return TRUE;
}

// 启动后立即探针: 屏幕中心 hit-test + 顶层窗口树
void WallApp::ProbeInput() {
    if (!g_probe) return;
    POINT pt{};
    pt.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    pt.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    HWND hit = WindowFromPoint(pt);
    Log("WindowFromPoint(center)=" + ClassNameOf(hit));
    if (hit) {
        HWND root = GetAncestor(hit, GA_ROOT);
        Log("  root=" + ClassNameOf(root));
    }

    // UIA 图标命中自检: 直接枚举桌面列表视图子项 (图标), 在其几何中心做 accHitTest,
    // 验证"点图标放行"依赖的完整通路 (不受其他窗口遮挡影响)
    {
        HWND progman = FindWindowW(L"Progman", nullptr);
        HWND dv = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
        HWND lv = dv ? FindWindowExW(dv, nullptr, L"SysListView32", nullptr) : nullptr;
        Log("uia selftest: lv=" + ClassNameOf(lv));
        if (lv) {
            IAccessible* acc = nullptr;
            HRESULT hr = AccessibleObjectFromWindow(lv, OBJID_CLIENT, __uuidof(IAccessible),
                reinterpret_cast<void**>(&acc));
            if (SUCCEEDED(hr) && acc) {
                long count = 0;
                acc->get_accChildCount(&count);
                Log("uia selftest: childCount=" + std::to_string(count));
                int tested = 0;
                // UIA 自检: 直接查找桌面列表视图的子元素 (图标), 并在首个图标中心
                // 做 ElementFromPoint (与生产命中同源)
                IUIAutomation* uia = Uia();
                if (uia) {
                    // 扫描所有含 DefView 的顶层窗口, 找真正持有图标 (ListItem>0) 的列表视图
                    IUIAutomationElement* listEl = nullptr;
                    for (HWND tw = GetTopWindow(nullptr); tw && !listEl;
                         tw = GetWindow(tw, GW_HWNDNEXT)) {
                        HWND dv2 = FindWindowExW(tw, nullptr, L"SHELLDLL_DefView", nullptr);
                        HWND lv2 = dv2 ? FindWindowExW(dv2, nullptr, L"SysListView32", nullptr)
                                       : nullptr;
                        if (!lv2) continue;
                        IUIAutomationElement* cand = nullptr;
                        uia->ElementFromHandle(lv2, &cand);
                        if (!cand) continue;
                        VARIANT vCt0;
                        VariantInit(&vCt0);
                        vCt0.vt = VT_I4;
                        vCt0.lVal = UIA_ListItemControlTypeId;
                        IUIAutomationCondition* c0 = nullptr;
                        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vCt0, &c0);
                        IUIAutomationElementArray* a0 = nullptr;
                        if (c0) {
                            cand->FindAll(TreeScope_Descendants, c0, &a0);
                            c0->Release();
                        }
                        int n0 = 0;
                        if (a0) a0->get_Length(&n0);
                        if (a0) a0->Release();
                        char bb[176];
                        sprintf_s(bb, "uia selftest: DefView of %s -> ListItems=%d",
                            ClassNameOf(tw).c_str(), n0);
                        Log(bb);
                        if (n0 > 0) listEl = cand;
                        else cand->Release();
                    }
                    if (listEl) {
                            VARIANT vCt;
                            VariantInit(&vCt);
                            vCt.vt = VT_I4;
                            vCt.lVal = UIA_ListItemControlTypeId;
                            IUIAutomationCondition* itemCond = nullptr;
                            uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vCt, &itemCond);
                            IUIAutomationElementArray* arr = nullptr;
                            if (itemCond) {
                                listEl->FindAll(TreeScope_Descendants, itemCond, &arr);
                                itemCond->Release();
                            }
                            int n = 0;
                            if (arr) arr->get_Length(&n);
                            char b[96];
                            sprintf_s(b, "uia selftest: desktop ListItems(UIA)=%d", n);
                            Log(b);
                            if (arr && n > 0) {
                                IUIAutomationElement* first = nullptr;
                                arr->GetElement(0, &first);
                                if (first) {
                                    RECT rc{};
                                    first->get_CurrentBoundingRectangle(&rc);
                                    POINT c{ rc.left + (rc.right - rc.left) / 2,
                                             rc.top + (rc.bottom - rc.top) / 2 };
                                    IUIAutomationElement* hitEl = nullptr;
                                    uia->ElementFromPoint(c, &hitEl);
                                    CONTROLTYPEID ct = 0;
                                    if (hitEl) {
                                        hitEl->get_CurrentControlType(&ct);
                                        hitEl->Release();
                                    }
                                    char b2[160];
                                    sprintf_s(b2,
                                        "uia selftest: item0 rect=(%d,%d %dx%d) center=(%d,%d) "
                                        "ElementFromPoint ct=%ld expect=%ld",
                                        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                        c.x, c.y, (long)ct, (long)UIA_ListItemControlTypeId);
                                    Log(b2);
                                    first->Release();
                                }
                            }
                            if (arr) arr->Release();
                            listEl->Release();
                    }
                } else {
                    Log("uia selftest: CoCreate CUIAutomation FAILED");
                }
                acc->Release();
            } else {
                Log("uia selftest: AccessibleObjectFromWindow FAILED hr=0x" +
                    std::to_string((unsigned long)hr));
            }
        }
    }
    LogWindowTree();

    // 可见性诊断 (进程内 C++ 枚举, 无 P/Invoke 委托 GC 问题):
    // DWM cloak 状态 + Progman 子窗 z 序 + 自身矩形
    {
        DWORD cloaked = 0;
        HRESULT hrCloak = DwmGetWindowAttribute(hwnd_, 14 /*DWMWA_CLOAKED*/,
            &cloaked, sizeof(cloaked));
        RECT rcSelf{};
        GetWindowRect(hwnd_, &rcSelf);
        char vb[220];
        sprintf_s(vb, "visibility: cloakHr=0x%08X cloaked=%lu selfRect=(%ld,%ld)-(%ld,%ld) %ldx%ld",
            (unsigned)hrCloak, cloaked, rcSelf.left, rcSelf.top, rcSelf.right, rcSelf.bottom,
            rcSelf.right - rcSelf.left, rcSelf.bottom - rcSelf.top);
        Log(vb);
        HWND progman = FindWindowW(L"Progman", nullptr);
        Log("visibility: progman=" + ClassNameOf(progman));
        VisCtx vctx{ 0 };
        if (progman) EnumChildWindows(progman, VisChildEnum, reinterpret_cast<LPARAM>(&vctx));
        Log("visibility: enum done children=" + std::to_string(vctx.i));
    }
}

void WallApp::NotePointer(UINT kind, POINT pt, WPARAM extra) {
    if (!g_probe) return;
    // 节流: 位置没变的重复事件只记一次; 总量前 60 条全记, 之后每 50 条记 1 条
    if (pt.x == lastLogX_ && pt.y == lastLogY_ && kind == WM_MOUSEMOVE) return;
    ++probeEvents_;
    if (probeEvents_ > 60 && (probeEvents_ % 50) != 0) return;
    lastLogX_ = pt.x; lastLogY_ = pt.y;
    HWND hit = WindowFromPoint(pt);
    char buf[160];
    sprintf_s(buf, "ptr msg=0x%04X pt=(%ld,%ld) wp=0x%04X hit=",
        kind, pt.x, pt.y, (unsigned)extra);
    Log(std::string(buf) + ClassNameOf(hit));
}

// ---- 托盘 ----
void WallApp::SetupTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayUid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Agent-Heart 动效壁纸");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        trayAdded_ = true;
        Log("tray icon added");
    } else {
        Log("tray icon add FAILED err=" + std::to_string(GetLastError()));
    }
}

void WallApp::RemoveTray() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayUid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void WallApp::ShowTrayMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kIdSettings, L"设置(&S)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kIdExit, L"退出壁纸");
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

// ---------------- 自动化验收截图: 把当前帧合成表面 (场景+覆盖层) 存为 PNG ----------------
bool CaptureShotToPngLegacy(ID2D1DeviceContext* dc, const std::wstring& path) {
    if (!dc || path.empty()) return false;
    D2D1_SIZE_U px = dc->GetPixelSize();
    if (px.width == 0 || px.height == 0) return false;
    if (g_shotBmp) {
        D2D1_SIZE_U cur = g_shotBmp->GetPixelSize();
        if (cur.width != px.width || cur.height != px.height) g_shotBmp = nullptr;
    }
    if (!g_shotBmp) {
        float dpiX = 96.0f, dpiY = 96.0f;
        dc->GetDpi(&dpiX, &dpiY);
        if (dpiX <= 0.0f) dpiX = 96.0f;   // 插值 DC 的 GetDpi 可能返回 0 -> E_INVALIDARG
        if (dpiY <= 0.0f) dpiY = 96.0f;
        D2D1_PIXEL_FORMAT fmt = D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE, fmt, dpiX, dpiY);
        HRESULT hr1 = dc->CreateBitmap(px, nullptr, 0, &props, g_shotBmp.put());
        if (FAILED(hr1)) {
            props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ, fmt, dpiX, dpiY);
            HRESULT hr2 = dc->CreateBitmap(px, nullptr, 0, &props, g_shotBmp.put());
            if (FAILED(hr2)) {
                char b[128];
                sprintf_s(b, "shot: CreateBitmap failed hr1=0x%08X hr2=0x%08X",
                    (unsigned)hr1, (unsigned)hr2);
                Log(b);
                g_shotDone = true;   // 不再每帧重试刷屏
                return false;
            }
        }
    }
    if (FAILED(g_shotBmp->CopyFromRenderTarget(nullptr, dc, nullptr))) {
        Log("shot: CopyFromRenderTarget failed");
        return false;
    }
    D2D1_MAPPED_RECT map{};
    if (FAILED(g_shotBmp->Map(D2D1_MAP_OPTIONS_READ, &map))) {
        Log("shot: Map failed");
        return false;
    }
    const GUID kPng = { 0x1fc87a51, 0x496a, 0x46a0,
        { 0x8b, 0x5b, 0xc8, 0x8e, 0xb8, 0x6f, 0x68, 0xd2 } };
    const GUID kBgra = { 0x6fddc324, 0x4e03, 0x4bfe,
        { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
    const GUID kFactory = { 0x317d06e8, 0x5f24, 0x433d,
        { 0xbd, 0xdf, 0x79, 0xc6, 0x40, 0x90, 0xb0, 0x29 } };
    bool ok = false;
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(kFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic))) && wic) {
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IWICBitmap* wb = nullptr;
        if (SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(kPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(wic->CreateBitmapFromMemory(px.width, px.height, kBgra,
                map.pitch, map.pitch * px.height, map.bits, &wb)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->WriteSource(wb, nullptr)) &&
            SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(enc->Commit())) {
            ok = true;
        }
        if (frame) frame->Release();
        if (wb) wb->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        wic->Release();
    }
    g_shotBmp->Unmap();
    Log(ok ? ("shot saved: " + Narrow(path)) : "shot: WIC encode FAILED");
    return ok;
}

// ---------------- 自动化验收截图 v2: D3D11 staging 回读 ----------------
// 合成插值 DC 上 CreateBitmap(CPU_READ) 返回 E_INVALIDARG (实测), 改走:
// dc.Flush -> GetTarget -> ID2D1Bitmap1.GetDXGISurface -> ID3D11Texture2D
// -> 同设备 staging CopyResource -> Map -> WIC PNG。
bool CaptureShotToPng(ID2D1DeviceContext* dc, const std::wstring& path,
                      ID2D1Bitmap1* overrideTarget = nullptr) {
    if (!dc || path.empty()) return false;
    Log("shot: enter capture");
    dc->Flush();   // D2D 批量命令先落地, 否则读到的是上一帧
    Log("shot: flushed");

    winrt::com_ptr<ID2D1Bitmap1> target;
    if (overrideTarget) {
        target.copy_from(overrideTarget);   // --snapshot: 直接导出快照本体
    } else {
        winrt::com_ptr<ID2D1Image> img;
        dc->GetTarget(img.put());
        target = img.try_as<ID2D1Bitmap1>();
    }
    if (!target) {
        Log("shot: GetTarget is not ID2D1Bitmap1");
        g_shotDone = true;
        return false;
    }
    winrt::com_ptr<IDXGISurface> dxgi;
    HRESULT hr = target->GetSurface(dxgi.put());
    if (FAILED(hr) || !dxgi) {
        char b[96];
        sprintf_s(b, "shot: GetSurface failed hr=0x%08X", (unsigned)hr);
        Log(b);
        g_shotDone = true;
        return false;
    }
    Log("shot: surface ok");
    winrt::com_ptr<ID3D11Texture2D> tex = dxgi.try_as<ID3D11Texture2D>();
    if (!tex) {
        Log("shot: QI ID3D11Texture2D failed");
        g_shotDone = true;
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    Log("shot: tex ok " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height));

    // 从纹理自身取所属 D3D11 设备 (最可靠)
    winrt::com_ptr<ID3D11Device> dev;
    tex->GetDevice(dev.put());
    if (!dev) {
        Log("shot: get ID3D11Device failed");
        g_shotDone = true;
        return false;
    }

    if (g_shotStage) {
        D3D11_TEXTURE2D_DESC old{};
        g_shotStage->GetDesc(&old);
        if (old.Width != desc.Width || old.Height != desc.Height) g_shotStage = nullptr;
    }
    if (!g_shotStage) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = desc.Width;
        sd.Height = desc.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateTexture2D(&sd, nullptr, g_shotStage.put());
        if (FAILED(hr)) {
            char b[96];
            sprintf_s(b, "shot: staging create failed hr=0x%08X", (unsigned)hr);
            Log(b);
            g_shotDone = true;
            return false;
        }
        dev->GetImmediateContext(g_shotCtx.put());
    }
    if (!g_shotCtx) {
        Log("shot: no immediate context");
        g_shotDone = true;
        return false;
    }

    Log("shot: staging ready");
    g_shotCtx->CopyResource(g_shotStage.get(), tex.get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_shotCtx->Map(g_shotStage.get(), 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
        Log("shot: Map failed");
        g_shotDone = true;
        return false;
    }
    Log("shot: mapped");

    // WIC GUID 一律用 wincodec.h 官方符号 (windowscodecs.lib 提供), 硬编码值实测未注册
    bool ok = false;
    IWICImagingFactory* wic = nullptr;
    HRESULT hrW = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic));
    if (FAILED(hrW) || !wic) {
        char b[96];
        sprintf_s(b, "shot: WIC CoCreate failed hr=0x%08X", (unsigned)hrW);
        Log(b);
    } else {
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IWICBitmap* wb = nullptr;
        do {
            hrW = wic->CreateStream(&stream);
            if (FAILED(hrW)) { Log("shot: WIC CreateStream failed"); break; }
            hrW = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
            if (FAILED(hrW)) {
                char b[96];
                sprintf_s(b, "shot: WIC InitFile failed hr=0x%08X", (unsigned)hrW);
                Log(b);
                break;
            }
            hrW = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
            if (FAILED(hrW)) {
                char b[96];
                sprintf_s(b, "shot: WIC CreateEncoder(PNG) failed hr=0x%08X", (unsigned)hrW);
                Log(b);
                break;
            }
            hrW = enc->Initialize(stream, WICBitmapEncoderNoCache);
            if (FAILED(hrW)) {
                char b[96];
                sprintf_s(b, "shot: WIC enc init failed hr=0x%08X", (unsigned)hrW);
                Log(b);
                break;
            }
            hrW = wic->CreateBitmapFromMemory(desc.Width, desc.Height,
                GUID_WICPixelFormat32bppBGRA, m.RowPitch, m.RowPitch * desc.Height,
                reinterpret_cast<BYTE*>(m.pData), &wb);
            if (FAILED(hrW)) {
                char b[96];
                sprintf_s(b, "shot: WIC FromMemory failed hr=0x%08X", (unsigned)hrW);
                Log(b);
                break;
            }
            hrW = enc->CreateNewFrame(&frame, nullptr);
            if (FAILED(hrW)) { Log("shot: WIC NewFrame failed"); break; }
            hrW = frame->Initialize(nullptr);
            if (FAILED(hrW)) { Log("shot: WIC frame init failed"); break; }
            hrW = frame->WriteSource(wb, nullptr);
            if (FAILED(hrW)) {
                char b[96];
                sprintf_s(b, "shot: WIC WriteSource failed hr=0x%08X", (unsigned)hrW);
                Log(b);
                break;
            }
            hrW = frame->Commit();
            if (FAILED(hrW)) { Log("shot: WIC frame commit failed"); break; }
            hrW = enc->Commit();
            if (FAILED(hrW)) { Log("shot: WIC enc commit failed"); break; }
            ok = true;
        } while (false);
        if (frame) frame->Release();
        if (wb) wb->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        wic->Release();
    }
    g_shotCtx->Unmap(g_shotStage.get(), 0);
    if (ok) {
        Log("shot saved: " + Narrow(path));
    } else {
        Log("shot: WIC encode FAILED");
        g_shotDone = true;   // 确定性失败不重试
    }
    return ok;
}

// ---- 帧循环 ----
void WallApp::OnFrameTick() {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;   // 规格 §5.1

    ui_.Tick(dt);                                  // 覆盖层动画推进
    EnsureAppsLoaded();                            // 首次开抽屉时加载应用目录
    PollTaskData(dt);                              // 任务面板打开期间 2s 轮询
    sphere_.SetSnapshotEnabled(ui_.AnyVisible() ||
        (!g_shotPath.empty() && !g_shotDone));      // 毛玻璃快照按需开启

    // 设置 -> 场景 (每帧同步: 设置窗口改动下一帧即生效; 未变化时各 Set 内部早退)
    {
        const AppConfig& cfg = ConfigGet();
        sphere_.SetAnimationsEnabled(cfg.animationsEnabled);
        sphere_.SetSphereScale(cfg.sphereScale);
        sphere_.SetClickEffectEnabled(cfg.clickEffectEnabled);
        sphere_.SetTrailEnabled(cfg.mouseTrailEnabled);
    }

    // 自证截图 (--selfshot): 进程内 PrintWindow 抓自身窗口 (PW_RENDERFULLCONTENT)
    if (!g_selfShotPath.empty() && !g_selfShotDone && sphere_.TickCount() >= 60) {
        g_selfShotDone = true;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int w = rc.right, h = rc.bottom;
        if (w > 0 && h > 0) {
            HDC hdcScreen = GetDC(hwnd_);
            HDC memDC = CreateCompatibleDC(hdcScreen);
            // 顶向下 32bpp DIB, 便于 WIC 直接编码 (BITMAPINFO 包含头)
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* dibBits = nullptr;
            HBITMAP topDown = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &dibBits,
                nullptr, 0);
            bool ok = false;
            if (topDown && dibBits) {
                HGDIOBJ old = SelectObject(memDC, topDown);
                ok = PrintWindow(hwnd_, memDC, 2 /*PW_RENDERFULLCONTENT*/);
                SelectObject(memDC, old);
                if (ok) {
                    // WIC 编码 (BGRA 顶向下, stride=w*4)
                    const GUID kPng2 = { 0x1fc87a51, 0x496a, 0x46a0,
                        { 0x8b, 0x5b, 0xc8, 0x8e, 0xb8, 0x6f, 0x68, 0xd2 } };
                    const GUID kBgra2 = { 0x6fddc324, 0x4e03, 0x4bfe,
                        { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
                    const GUID kFac2 = { 0x317d06e8, 0x5f24, 0x433d,
                        { 0xbd, 0xdf, 0x79, 0xc6, 0x40, 0x90, 0xb0, 0x29 } };
                    IWICImagingFactory* wic = nullptr;
                    if (SUCCEEDED(CoCreateInstance(kFac2, nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic))) && wic) {
                        IWICStream* stream = nullptr;
                        IWICBitmapEncoder* enc = nullptr;
                        IWICBitmapFrameEncode* frame = nullptr;
                        IWICBitmap* wb = nullptr;
                        if (SUCCEEDED(wic->CreateStream(&stream)) &&
                            SUCCEEDED(stream->InitializeFromFilename(g_selfShotPath.c_str(), GENERIC_WRITE)) &&
                            SUCCEEDED(wic->CreateEncoder(kPng2, nullptr, &enc)) &&
                            SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
                            SUCCEEDED(wic->CreateBitmapFromMemory(w, h, kBgra2,
                                w * 4, (SIZE_T)w * 4 * h,
                                reinterpret_cast<BYTE*>(dibBits), &wb)) &&
                            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                            SUCCEEDED(frame->Initialize(nullptr)) &&
                            SUCCEEDED(frame->WriteSource(wb, nullptr)) &&
                            SUCCEEDED(frame->Commit()) &&
                            SUCCEEDED(enc->Commit())) {
                            Log("selfshot saved: " + Narrow(g_selfShotPath));
                        } else {
                            Log("selfshot: WIC encode failed");
                        }
                        if (frame) frame->Release();
                        if (wb) wb->Release();
                        if (enc) enc->Release();
                        if (stream) stream->Release();
                        wic->Release();
                    }
                } else {
                    Log("selfshot: PrintWindow failed err=" + std::to_string(GetLastError()));
                }
            }
            if (topDown) DeleteObject(topDown);
            DeleteDC(memDC);
            ReleaseDC(hwnd_, hdcScreen);
        }
    }

    // 自测输入链路 (--selftest-input): 由帧计数驱动的注入序列
    if (g_selfTest) {
        int tc = sphere_.TickCount();
        auto injectKey = [](BYTE vk, bool down) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
        };
        auto injectWheel = [](DWORD delta) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = delta;
            SendInput(1, &in, sizeof(INPUT));
        };
        if (tc == 30) {
            Log("selftest: injecting Win+D (show desktop)");
            injectKey(VK_LWIN, true); injectKey('D', true);
            injectKey('D', false); injectKey(VK_LWIN, false);
        } else if (tc == 55) {
            POINT c{ GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2 };
            GetCursorPos(&g_savedCursor);
            BOOL ok = SetCursorPos(c.x, c.y);
            Log(std::string("selftest: SetCursorPos center -> ") + (ok ? "OK" : "FAIL gle=") +
                (ok ? "" : std::to_string(GetLastError())));
            if (ok) {
                injectWheel(120);
                Log("selftest: injected wheel-up x1");
            }
        } else if (tc == 90) {
            char b[120];
            sprintf_s(b, "selftest: llEventsSeen=%d (need >0 to prove hook delivery)", g_llSeen);
            Log(b);
        } else if (tc == 110) {
            injectKey(VK_LWIN, true); injectKey('D', true);
            injectKey('D', false); injectKey(VK_LWIN, false);
            SetCursorPos(g_savedCursor.x, g_savedCursor.y);
            Log("selftest: restored desktop + cursor; done");
            g_selfTest = false;
        }
    }

    // 自动化验收触发器 (--open/--interact), 由帧计数驱动
    {
        int tc = sphere_.TickCount();
        if (!g_open.empty() && tc == 5) {
            if (g_open == "drawer") ui_.OpenDrawer();
            else if (g_open == "tasks") ui_.ToggleTaskPanel();
        }
        if (!g_interact.empty() && tc == 30) {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            float s = dpiScale_ > 0 ? dpiScale_ : 1.0f;
            float cx = (rc.right * 0.5f) / s, cy = (rc.bottom * 0.5f) / s;
            if (g_interact == "hover") sphere_.PointerMove(cx, cy, false);
            else if (g_interact == "click") { sphere_.PointerDown(cx, cy); sphere_.PointerUp(cx, cy); }
            else if (g_interact == "drag") sphere_.PointerDown(cx, cy);
        }
        // cycle: 无输入依赖的状态机全路径自测 (开/ Esc关/ 拖拽关/ 面板开关/ CloseAll)
        if (g_interact == "cycle") {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            float s = dpiScale_ > 0 ? dpiScale_ : 1.0f;
            float cx = (rc.right * 0.5f) / s;
            if (tc == 5) {
                ui_.OpenDrawer();
                Log("cycle: open drawer");
            } else if (tc == 25) {
                bool c = ui_.OnEsc();
                Log("cycle: OnEsc consumed=" + std::to_string(c));
            } else if (tc == 35) {
                ui_.OpenDrawer();
                Log("cycle: reopen drawer");
            } else if (tc == 50) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = cx; e.y = 8.f;   // 头部最上沿 (tab 上方)
                bool c = ui_.OnPointer(e);
                Log("cycle: headDown consumed=" + std::to_string(c));
            } else if (tc == 54) {
                PointerEvent e;
                e.kind = PtrKind::Move;
                e.x = cx; e.y = 128.f; e.down = true;
                ui_.OnPointer(e);
            } else if (tc == 58) {
                PointerEvent e;
                e.kind = PtrKind::Up;
                e.x = cx; e.y = 128.f;
                ui_.OnPointer(e);
                Log("cycle: head drag released dy=120>90");
            } else if (tc == 60) {
                ui_.ToggleTaskPanel();
                Log("cycle: toggle taskpanel");
            } else if (tc == 65) {
                ui_.CloseAll();
                Log("cycle: CloseAll");
            }
        }
        // tabtest: 点第二个 tab (未分类), 末帧应见激活胶囊从 tab1 移到 tab2
        if (g_interact == "tabtest") {
            if (tc == 5) {
                ui_.OpenDrawer();
                Log("tabtest: open drawer");
            } else if (tc == 30) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = 170.f; e.y = 40.f;   // tab2 (未分类) 中部, tab1 约止于 x=125
                bool c = ui_.OnPointer(e);
                Log("tabtest: down@tab2 consumed=" + std::to_string(c));
            } else if (tc == 33) {
                PointerEvent e;
                e.kind = PtrKind::Up;
                e.x = 170.f; e.y = 40.f;
                ui_.OnPointer(e);
                Log("tabtest: up@tab2 done");
            }
        }
        // tilelaunch: 点第一行第一列磁贴, 验证 命中 -> ShellExecute -> 自动关抽屉
        if (g_interact == "tilelaunch") {
            if (tc == 5) {
                ui_.OpenDrawer();
                Log("tilelaunch: open drawer");
            } else if (tc == 50) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = 92.f; e.y = 140.f;   // tile1: x=[32,152] y≈[93,216]
                bool c = ui_.OnPointer(e);
                Log("tilelaunch: down@tile1 consumed=" + std::to_string(c));
            } else if (tc == 53) {
                PointerEvent e;
                e.kind = PtrKind::Up;
                e.x = 92.f; e.y = 140.f;
                ui_.OnPointer(e);
                Log("tilelaunch: up@tile1 done");
            }
        }
        // editmode: 编辑按钮切换 -> 输入框聚焦 -> 键入 -> 回车建分类 (全链路)
        if (g_interact == "editmode") {
            if (tc == 5) {
                ui_.OpenDrawer();
                Log("editmode: open drawer");
            } else if (tc == 30) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = 2432.f; e.y = 40.f;   // 编辑按钮 (右上, 宽约64)
                bool c1 = ui_.OnPointer(e);
                e.kind = PtrKind::Up;
                bool c2 = ui_.OnPointer(e);
                Log("editmode: edit-btn consumed down=" + std::to_string(c1) +
                    " up=" + std::to_string(c2));
            } else if (tc == 45) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = 162.f; e.y = 100.f;   // 新分类输入框 (head71 + editbar12 + 半高)
                bool c1 = ui_.OnPointer(e);
                e.kind = PtrKind::Up;
                bool c2 = ui_.OnPointer(e);
                Log("editmode: input consumed down=" + std::to_string(c1) +
                    " up=" + std::to_string(c2));
            } else if (tc == 55) {
                bool k1 = ui_.OnKey(0x58, L'X', true);    // 'X'
                bool k2 = ui_.OnKey(0x58, L'X', false);
                Log("editmode: key X consumed=" + std::to_string(k1) +
                    "/" + std::to_string(k2));
            } else if (tc == 65) {
                bool k1 = ui_.OnKey(0x0D, L'\r', true);   // Enter 提交
                bool k2 = ui_.OnKey(0x0D, L'\r', false);
                Log("editmode: Enter consumed=" + std::to_string(k1) +
                    "/" + std::to_string(k2));
            }
        }
        // n2state: 球体交互状态级验证 (悬停进/点击进/滚轮倒放/悬停出) +
        //          部分拖拽分支 (dy<=90 不关闭, 弹簧回位)
        if (g_interact == "n2state") {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            float s = dpiScale_ > 0 ? dpiScale_ : 1.0f;
            float cx = (rc.right * 0.5f) / s, cy = (rc.bottom * 0.5f) / s;
            if (tc == 10) {
                sphere_.PointerMove(cx, cy, false);
                Log("n2: hover-move center");
            } else if (tc == 15) {
                Log("n2: hoverActive=" + std::to_string(sphere_.HoverActive()) +
                    " (expect 1)");
            } else if (tc == 30) {
                sphere_.PointerDown(cx, cy);
            } else if (tc == 31) {
                sphere_.PointerUp(cx, cy);
            } else if (tc == 36) {
                Log("n2: clickActive=" + std::to_string(sphere_.ClickActive()) +
                    " hover=" + std::to_string(sphere_.HoverActive()) +
                    " (click expect 1)");
            } else if (tc == 45) {
                sphere_.Wheel(120);   // DOM 正值 = 下滑 = 倒放
            } else if (tc == 50) {
                Log("n2: afterWheel clickActive=" + std::to_string(sphere_.ClickActive()) +
                    " (expect 0)");
            } else if (tc == 55) {
                sphere_.PointerMove(-1.0e6f, -1.0e6f, false);
            } else if (tc == 60) {
                Log("n2: afterLeave hoverActive=" + std::to_string(sphere_.HoverActive()) +
                    " (expect 0)");
            } else if (tc == 65) {
                ui_.OpenDrawer();
                Log("n2: drawer open for partial drag");
            } else if (tc == 70) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = cx; e.y = 8.f;
                ui_.OnPointer(e);
            } else if (tc == 74) {
                PointerEvent e;
                e.kind = PtrKind::Move;
                e.x = cx; e.y = 48.f; e.down = true;   // dy=40 <= 90
                ui_.OnPointer(e);
            } else if (tc == 78) {
                PointerEvent e;
                e.kind = PtrKind::Up;
                e.x = cx; e.y = 48.f;
                ui_.OnPointer(e);
            } else if (tc == 82) {
                Log("n2: partialDrag dy=40 drawerOpen=" +
                    std::to_string(ui_.DrawerOpen()) + " (expect 1, spring back)");
            }
        }
        // rowclick: 任务行点击 -> onFocus 回调 -> 面板自动关闭 (spec §3.8)
        if (g_interact == "rowclick") {
            if (tc == 5) {
                ui_.ToggleTaskPanel();
                Log("rowclick: open taskpanel");
            } else if (tc == 40) {
                PointerEvent e;
                e.kind = PtrKind::Down;
                e.x = 1248.f; e.y = 267.f;   // row1 中心 (DIP): 行区 y≈236-299
                bool c1 = ui_.OnPointer(e);
                e.kind = PtrKind::Up;
                bool c2 = ui_.OnPointer(e);
                Log("rowclick: row1 consumed down=" + std::to_string(c1) +
                    " up=" + std::to_string(c2));
            } else if (tc == 70) {
                // 点击在 tc40; 关闭动画 ~0.3s=18帧, tc70 (0.5s 后) 应已完成
                Log("rowclick: tasksOpen=" + std::to_string(ui_.TasksOpen()) +
                    " drawerOpen=" + std::to_string(ui_.DrawerOpen()) +
                    " (expect 0/0 = 自动关闭完成)");
            }
        }
        if (g_interact == "drag" && tc == 25) {
            float px, py, pz;
            sphere_.CameraPosition(px, py, pz);
            char db[140];
            sprintf_s(db, "drag: camBefore=(%.4f,%.4f,%.4f)", px, py, pz);
            Log(db);
        }
        if (g_interact == "drag" && tc == 45) {
            RECT rc{}; GetClientRect(hwnd_, &rc);
            float s = dpiScale_ > 0 ? dpiScale_ : 1.0f;
            sphere_.PointerMove((rc.right * 0.62f) / s, (rc.bottom * 0.5f) / s, true);
        }
        if (g_interact == "drag" && tc == 60) {
            float px, py, pz;
            sphere_.CameraPosition(px, py, pz);
            char db[140];
            sprintf_s(db, "drag: camAfter=(%.4f,%.4f,%.4f) (expect changed)", px, py, pz);
            Log(db);
        }
        if (g_interact == "drag" && tc == 50) {
            RECT rc{}; GetClientRect(hwnd_, &rc);
            float s = dpiScale_ > 0 ? dpiScale_ : 1.0f;
            sphere_.PointerUp((rc.right * 0.62f) / s, (rc.bottom * 0.5f) / s);
        }
    }

    sphere_.Tick(dt);
    if (sphere_.TickCount() == 60)
        Log("frame loop OK: ticks=60 frames=" + std::to_string(sphere_.FrameCount()));

    // 心跳: 每 600 帧 (~10s) 记录持续健康状态 (ticks-frames = 掉帧数)
    {
        static int hb = 0;
        static auto hbStart = std::chrono::steady_clock::now();
        if (++hb % 600 == 0) {
            auto up = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - hbStart).count();
            double ticksSoFar = (double)hb;   // hb 即心跳时的 tick 计数
            char b[200];
            sprintf_s(b, "heartbeat uptime=%.1fs ticks=%d frames=%d dropped=%d avgTickMs=%.1f",
                up, sphere_.TickCount(), sphere_.FrameCount(),
                sphere_.TickCount() - sphere_.FrameCount(),
                up * 1000.0 / ticksSoFar);
            Log(b);
        }
    }
}

// 首次打开抽屉时加载一次 (原 Drawer.tsx loadedRef 语义)
void WallApp::EnsureAppsLoaded() {
    if (appsLoaded_ || !ui_.DrawerOpen()) return;
    data_.LoadApps();
    ui_.SetApps(data_.Apps(), data_.Categories());
    appsLoaded_ = true;
    Log("apps loaded: " + std::to_string(data_.Apps().size()) +
        " cats=" + std::to_string(data_.Categories().size()));
}

// 任务面板打开期间每 2s 刷新 (原 TaskPanel.tsx 2000ms 轮询, 首次立即)
void WallApp::PollTaskData(double dt) {
    if (!ui_.TasksOpen()) { tasksPrimed_ = false; taskPoll_ = 0; return; }
    taskPoll_ += dt;
    if (tasksPrimed_ && taskPoll_ < 2.0) return;
    tasksPrimed_ = true;
    taskPoll_ = 0;
    std::vector<TaskItem> tasks;
    data_.RefreshTasks(tasks);
    ui_.SetTasks(tasks);
    ui_.SetStats(data_.QueryStats());
}

// ---- 初始化 ----
bool WallApp::Initialize(HINSTANCE inst) {
    try {
        winrt::init_apartment(apartment_type::single_threaded);
    } catch (hresult_error const& e) {
        LogEx("init_apartment", e);
        return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc)) return false;

    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (g_fullscreen) {
        hwnd_ = CreateWindowExW(ex, kClassName, L"Agent-Heart",
            WS_POPUP | WS_VISIBLE, x, y, w, h, nullptr, nullptr, inst, this);
    } else {
        hwnd_ = CreateWindowExW(0, kClassName, L"Agent-Heart (windowed test)",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
            nullptr, nullptr, inst, this);
    }
    if (!hwnd_) return false;

    try {
        DispatcherQueueOptions dq{};
        dq.dwSize = sizeof(DispatcherQueueOptions);
        dq.threadType = 2;
        dq.apartmentType = 0;
        check_hresult(CreateDispatcherQueueController(dq, &dqController_));
        Log("dispatcher queue created");

        compositor_ = Compositor();
        auto interop = compositor_.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        check_hresult(interop->CreateDesktopWindowTarget(hwnd_, FALSE, &target_));

        root_ = compositor_.CreateSpriteVisual();
        Color black{};
        black.A = 255; black.R = 0; black.G = 0; black.B = 0;
        auto bg = compositor_.CreateColorBrush(black);
        root_.Brush(bg);
        SetRootVisual();

        if (sphere_.Init(compositor_, 0.0f, 0.0f)) {
            root_.Children().InsertAtTop(sphere_.Visual());
            Log("sphere scene attached (768 particles, K=12)");
        } else {
            Log("sphere scene init FAILED");
        }
        Layout();   // 关键: 壁纸模式下 WM_SIZE 可能早于 Init, 这里补建绘制表面

        try {
            auto dq2 = Windows::System::DispatcherQueueController{ nullptr };
            check_hresult(reinterpret_cast<::IUnknown*>(dqController_)->QueryInterface(
                winrt::guid_of<winrt::Windows::System::DispatcherQueueController>(),
                reinterpret_cast<void**>(winrt::put_abi(dq2))));
            frameTimer_ = dq2.DispatcherQueue().CreateTimer();
            // 回调实测耗时 ~6.5ms, 定时器按完成点起算: 10ms + 6.5ms ≈ 16.5ms ≈ 60fps
            // (16ms 时实测 22.5ms/帧 ≈ 44fps; 默认 15.625ms 网格下 16ms 会退到 31fps)
            frameTimer_.Interval(std::chrono::milliseconds(10));
            frameTimer_.Tick([this](Windows::System::DispatcherQueueTimer const&,
                winrt::Windows::Foundation::IInspectable const&) {
                OnFrameTick();
            });
            lastTick_ = std::chrono::steady_clock::now();
            frameTimer_.Start();
            Log("frame timer started (16ms)");
        } catch (hresult_error const& e) {
            LogEx("frame timer", e);
        }

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    } catch (hresult_error const& e) {
        LogEx("composition init", e);
        return false;
    }

    if (g_fullscreen) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!AttachToDesktop()) {
            Log("WorkerW attach FAILED -> staying as plain bottom window");
        }
    } else {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        Log("windowed mode");
    }

    // 覆盖层 <-> 数据层动作回调
    ui_.onLaunch = [this](const std::string& id) {
        std::string name, target;
        for (const auto& a : data_.Apps()) {
            if (a.id == id) { name = Narrow(a.name); target = Narrow(a.target); break; }
        }
        bool ok = data_.Launch(id);
        Log("launch id=" + id + " name=" + name + " target=" + target +
            " ok=" + (ok ? "1" : "0"));
        return ok;
    };
    ui_.onFocus = [this](const std::wstring& id) {
        bool ok = data_.FocusTask(id);
        Log("focusTask id=" + Narrow(id) + " ok=" + (ok ? "1" : "0"));
    };
    ui_.onCloseTask = [this](const std::wstring& id) { data_.CloseTask(id); };
    ui_.onSetCategory = [this](const std::string& id, const std::wstring& cat) {
        return data_.SetAppCategory(id, cat);
    };
    ui_.onAddCategory = [this](const std::wstring& name) {
        bool ok = data_.AddCategory(name);
        Log("addCategory name=" + Narrow(name) + " ok=" + (ok ? "1" : "0"));
        return ok;
    };
    // 场景 -> 覆盖层绘制钩子 (SphereScene 在场景画完后调用)
    sphere_.SetOverlayDraw([this](ID2D1DeviceContext* dc, ID2D1Bitmap* snap, float w, float h) {
        static int tLogN = 0;
        if (tLogN < 3 && snap) {
            ++tLogN;
            D2D1_MATRIX_3X2_F t0;
            dc->GetTransform(&t0);
            float dx = 96, dy = 96;
            dc->GetDpi(&dx, &dy);
            char b0[220];
            sprintf_s(b0, "pre-ui: dip=%.0fx%.0f snapPx=%ux%u dpi=%.0f xform=(%.3f %.3f %.3f %.3f %.1f %.1f)",
                w, h, snap->GetPixelSize().width, snap->GetPixelSize().height, dx,
                t0._11, t0._12, t0._21, t0._22, t0._31, t0._32);
            Log(b0);
        }
        ui_.Draw(dc, snap, w, h);
        if (tLogN <= 3 && snap) {
            D2D1_MATRIX_3X2_F t1;
            dc->GetTransform(&t1);
            char b1[200];
            sprintf_s(b1, "post-ui: xform=(%.3f %.3f %.3f %.3f %.1f %.1f)",
                t1._11, t1._12, t1._21, t1._22, t1._31, t1._32);
            Log(b1);
        }
        static int snapLogN = 0;
        if (snapLogN < 5 && ui_.AnyVisible()) {
            ++snapLogN;
            Log(std::string("overlay draw: snapshot=") + (snap ? "present" : "NULL"));
        }
        if (!g_snapPath.empty() && !g_snapDone && snap &&
            sphere_.TickCount() >= kShotFrame) {
            g_snapDone = CaptureShotToPng(dc, g_snapPath,
                static_cast<ID2D1Bitmap1*>(snap));
        }
        if (!g_shotPath.empty() && !g_shotDone &&
            sphere_.TickCount() >= kShotFrame) {
            if (CaptureShotToPng(dc, g_shotPath)) g_shotDone = true;
        }
    });

    g_app = this;
    g_uiLog = [](const std::string& s) { Log(s); };
    InstallHooks();

    SetupTray();
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!attached_) ProbeInput();   // 挂载成功时 AttachToDesktop 内已探针
    Log("initialized OK");
    return true;
}

int WallApp::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WallApp::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WallApp* self = nullptr;
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<WallApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<WallApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT WallApp::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_PAINT:
        ValidateRect(hwnd_, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: {
        POINT pt{ (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        if (msg == WM_MOUSEWHEEL) {   // wheel 的坐标是屏幕坐标
            pt.x = (int)(short)LOWORD(lp);
            pt.y = (int)(short)HIWORD(lp);
            ScreenToClient(hwnd_, &pt);
        } else {
            // 相对坐标, 转屏幕便于探针对比
            ClientToScreen(hwnd_, &pt);
        }
        NotePointer(msg, pt, wp);
        return 0;
    }
    case kMsgTray:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt); ShowTrayMenu(pt);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdExit) {
            Log("exit requested via tray");
            DestroyWindow(hwnd_);
        } else if (LOWORD(wp) == kIdSettings) {
            Log("settings requested via tray");
            ShowSettingsWindow();        // 单实例: 已开则 SetForegroundWindow 置前
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !g_fullscreen) DestroyWindow(hwnd_);
        return 0;
    case WM_CLOSE:
        if (g_fullscreen) return 0;   // 壁纸模式: 关闭请求走托盘退出
        break;
    case WM_DESTROY:
        if (frameTimer_) frameTimer_.Stop();
        timeEndPeriod(1);
        RemoveHooks();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    default:
        if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
            // explorer 重启 -> 重新挂载 + 重加托盘
            Log("TaskbarCreated: re-attach");
            trayAdded_ = false;
            SetupTray();
            AttachToDesktop();
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ================= Stage2/3: 输入中枢 (低级鼠标/键盘钩子) =================
// 壁纸窗口挂在 Progman 的 DefView 之下, 系统鼠标消息到不了我们; 低级钩子全局可见,
// 按"窗口链属于桌面 or 自身"过滤路由。左键按下在空桌面时用 UIA 判断是否点在图标上:
// 点图标 -> 放行给系统; 空桌面 -> 路由给覆盖层/球体并吞掉 (防止图标框选)。

LRESULT CALLBACK WallApp::LLMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    bool swallow = false;
    if (nCode == HC_ACTION && g_app)
        swallow = g_app->HandleLLMouse(wp, reinterpret_cast<MSLLHOOKSTRUCT*>(lp));
    if (swallow) return 1;
    return CallNextHookEx(g_app ? g_app->llMouse_ : nullptr, nCode, wp, lp);
}

LRESULT CALLBACK WallApp::LLKeyProc(int nCode, WPARAM wp, LPARAM lp) {
    bool swallow = false;
    if (nCode == HC_ACTION && g_app)
        swallow = g_app->HandleLLKey(wp, reinterpret_cast<KBDLLHOOKSTRUCT*>(lp));
    if (swallow) return 1;
    return CallNextHookEx(g_app ? g_app->llKey_ : nullptr, nCode, wp, lp);
}

void WallApp::InstallHooks() {
    llMouse_ = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc, GetModuleHandleW(nullptr), 0);
    llKey_ = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyProc, GetModuleHandleW(nullptr), 0);
    Log(std::string("hooks: mouse=") + (llMouse_ ? "ok" : "FAIL") +
        " key=" + (llKey_ ? "ok" : "FAIL"));
}

void WallApp::RemoveHooks() {
    if (llMouse_) { UnhookWindowsHookEx(llMouse_); llMouse_ = nullptr; }
    if (llKey_) { UnhookWindowsHookEx(llKey_); llKey_ = nullptr; }
}

float WallApp::ToDipX(int screenX) const {
    POINT p{ screenX, 0 };
    ScreenToClient(hwnd_, &p);
    return static_cast<float>(p.x) / (dpiScale_ > 0.0f ? dpiScale_ : 1.0f);
}

float WallApp::ToDipY(int screenY) const {
    POINT p{ 0, screenY };
    ScreenToClient(hwnd_, &p);
    return static_cast<float>(p.y) / (dpiScale_ > 0.0f ? dpiScale_ : 1.0f);
}

// UIA: 光标是否点在桌面图标上。空桌面 = false; 检测失败经 checkFailed 反馈 (调用方不吞)。
bool WallApp::PointOverDesktopIcon(POINT pt, bool* checkFailed) const {
    if (checkFailed) *checkFailed = false;
    IUIAutomation* uia = Uia();
    if (!uia) {
        if (checkFailed) *checkFailed = true;
        return false;
    }
    // ElementFromPoint 与 WindowFromPoint 同一套命中: 调用前提 (root==desktop) 已由
    // 路由层保证, 因此返回的一定是桌面链上的元素
    IUIAutomationElement* el = nullptr;
    HRESULT hr = uia->ElementFromPoint(pt, &el);
    if (FAILED(hr) || !el) {
        if (checkFailed) *checkFailed = true;
        return false;
    }
    bool icon = false;
    for (int up = 0; up < 6 && el; ++up) {
        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        if (ct == UIA_ListItemControlTypeId) {   // 桌面图标 = ListItem
            icon = true;
            break;
        }
        BSTR cls = nullptr;
        el->get_CurrentClassName(&cls);
        if (cls) {
            bool isList = (wcscmp(cls, L"SysListView32") == 0);
            SysFreeString(cls);
            if (isList) break;   // 已到列表本身仍非项 -> 空桌面
        }
        IUIAutomationTreeWalker* walker = nullptr;
        uia->get_RawViewWalker(&walker);
        IUIAutomationElement* parent = nullptr;
        if (walker) {
            walker->GetParentElement(el, &parent);
            walker->Release();
        }
        el->Release();
        el = parent;
    }
    if (el) el->Release();
    return icon;
}

bool WallApp::IsDesktopForeground() const {
    HWND f = GetForegroundWindow();
    if (!f) return true;
    HWND r = GetAncestor(f, GA_ROOT);
    if (r == hwnd_) return true;
    wchar_t cls[64] = L"";
    if (r) GetClassNameW(r, cls, 64);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

void WallApp::RouteMove(float x, float y, bool down) {
    if (g_probe) { static int n = 0; if (++n <= 60) {
        char b[128]; sprintf_s(b, "route Move (%.0f,%.0f) down=%d", x, y, down ? 1 : 0); Log(b); } }
    PointerEvent e;
    e.kind = PtrKind::Move;
    e.x = x; e.y = y; e.down = down;
    if (ui_.BlocksSphere()) {
        ui_.OnPointer(e);
        if (ui_.HitInteractive(x, y)) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    } else {
        sphere_.PointerMove(x, y, down);
        sphere_.SetTrailPoint(x, y);   // 桌面区域路由: 鼠标拖尾 (光标静止时内部不新增)
    }
}

void WallApp::RouteDown(float x, float y) {
    if (g_probe) { static int n = 0; if (++n <= 60) {
        char b[128]; sprintf_s(b, "route Down (%.0f,%.0f)", x, y); Log(b); } }
    PointerEvent e;
    e.kind = PtrKind::Down;
    e.x = x; e.y = y;
    if (ui_.BlocksSphere()) ui_.OnPointer(e);
    else sphere_.PointerDown(x, y);
}

void WallApp::RouteUp(float x, float y) {
    if (g_probe) { static int n = 0; if (++n <= 60) {
        char b[128]; sprintf_s(b, "route Up (%.0f,%.0f)", x, y); Log(b); } }
    PointerEvent e;
    e.kind = PtrKind::Up;
    e.x = x; e.y = y;
    if (ui_.BlocksSphere()) ui_.OnPointer(e);
    else sphere_.PointerUp(x, y);
}

void WallApp::RouteWheel(float x, float y, int winDelta) {
    // win32 约定: 上滑为正; DOM 约定 (ui_overlay.h/前端规格): 上滑为负。
    // 全链路 (开抽屉/倒放/列表滚动) 均按 DOM 语义, 入口统一取负换算。
    int delta = -winDelta;
    if (g_probe) Log("route Wheel (" + std::to_string((int)x) + "," +
        std::to_string((int)y) + ") deltaDom=" + std::to_string(delta));
    if (ui_.AnyVisible()) {   // 覆盖层打开: 交给 UI (列表滚动等; 原页面无滚轮关抽屉)
        PointerEvent e;
        e.kind = PtrKind::Wheel;
        e.x = x; e.y = y; e.wheelDelta = delta;
        ui_.OnPointer(e);
        return;
    }
    if (delta < 0) {          // 桌面上滑 = 开抽屉唯一入口 (Viewport.ts:249)
        if (kOverlaysEnabled) ui_.OpenDrawer();
        return;                 // 关闭态: 上滑无动作 (用户要求只留壁纸动效)
    }
    if (delta > 0 && sphere_.ClickActive())
        sphere_.Wheel(delta); // 下滑倒放点击互换 (Viewport.ts:254)
}

bool WallApp::HandleLLMouse(WPARAM msg, MSLLHOOKSTRUCT* info) {
    if (!hwnd_) return false;
    ++g_llSeen;   // 自测判据: 注入的事件若到达钩子, 此计数增长
    const POINT pt = info->pt;
    if (g_probe) {
        // 按十进制原值打印; 移动限 8 条, 按键/滚轮类不限 (否则配额会被打字吃光)
        static int moves = 0;
        bool isMove = (msg == WM_MOUSEMOVE);
        if (!isMove || ++moves <= 8) {
            HWND hit0 = WindowFromPoint(pt);
            HWND root0 = hit0 ? GetAncestor(hit0, GA_ROOT) : nullptr;
            Log("llmouse msg=" + std::to_string((unsigned)msg) +
                (isMove ? "(MOVE)" : "") + " pt=(" +
                std::to_string(pt.x) + "," + std::to_string(pt.y) + ") hit=" +
                ClassNameOf(hit0) + " root=" + ClassNameOf(root0));
        }
    }

    // ---- 左键抬起: 优先闭合已开始的手势 (即使光标已移出桌面区域) ----
    if (msg == WM_LBUTTONUP) {
        if (gestureActive_) {
            gestureActive_ = false;
            RouteUp(ToDipX(pt.x), ToDipY(pt.y));
        }
        bool swallow = leftSwallowed_;
        leftSwallowed_ = false;
        return swallow;
    }

    HWND hit = WindowFromPoint(pt);
    if (!hit) return false;
    HWND root = GetAncestor(hit, GA_ROOT);
    bool ours = (hit == hwnd_) || IsChild(hwnd_, hit);
    bool desktop = false;
    if (!ours && root && root != hwnd_) {
        wchar_t cls[64] = L"";
        GetClassNameW(root, cls, 64);
        desktop = (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0);
    }
    // 手势进行中 (拖拽球体/面板) 时, 光标移出桌面区域也继续路由 (等价指针捕获)
    if (!ours && !desktop && !gestureActive_) {
        // spec B4 pointerleave 等价: 光标离开桌面区域 -> 以一个必然脱靶的坐标        // 触发一次 Move, 让 B3 射线未命中分支退出悬停形变
        if (sphere_.HoverActive()) sphere_.PointerMove(-1.0e6f, -1.0e6f, false);
        return false;
    }

    const float x = ToDipX(pt.x);
    const float y = ToDipY(pt.y);

    switch (msg) {
    case WM_MOUSEMOVE: {
        bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        RouteMove(x, y, down);
        break;
    }
    case WM_LBUTTONDOWN: {
        if (desktop) {
            bool failed = false;
            if (PointOverDesktopIcon(pt, &failed)) return false;  // 点图标: 系统处理
            RouteDown(x, y);
            gestureActive_ = true;
            if (!failed) {         // 空桌面: 吞掉, 防止图标框选/清除选择
                leftSwallowed_ = true;
                return true;
            }
        } else {
            RouteDown(x, y);
            gestureActive_ = true;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (!ours && !desktop) break;
        int delta = static_cast<short>(HIWORD(info->mouseData));
        RouteWheel(x, y, delta);   // 不吞: 桌面滚轮本无系统行为
        break;
    }
    default:
        break;
    }
    return false;
}

bool WallApp::HandleLLKey(WPARAM msg, KBDLLHOOKSTRUCT* info) {
    const DWORD vk = info->vkCode;
    const bool injected = (info->flags & LLKHF_INJECTED) != 0;
    const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    const bool up = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
    if (!down && !up) return false;
    if (g_probe) {
        // Esc/Win/Win+D 相关键不限条数 (验收关键), 其余键限 40 条
        static int n = 0;
        bool keyCritical = (vk == VK_ESCAPE || vk == VK_LWIN || vk == VK_RWIN || vk == 'D');
        if (keyCritical || ++n <= 40) {
            char kb[140];
            sprintf_s(kb, "llkey vk=0x%X %s%s fg=%s", (unsigned)vk,
                down ? "down" : "up", injected ? " injected" : "",
                IsDesktopForeground() ? "desktop" : "other");
            Log(kb);
        }
    }

    // 注入事件 (为组合键合成的 Win-down): 只记状态, 放行
    if (injected) return false;

    // ---- Esc: 覆盖层打开且桌面前台 -> 关顶层并吞 ----
    if (vk == VK_ESCAPE && down) {
        if (ui_.AnyVisible() && IsDesktopForeground()) {
            ui_.OnEsc();
            return true;
        }
        return false;
    }

    // ---- Win 键: 桌面前台 -> 裸 Win 切任务面板并吞; 否则放行 (Start 菜单照常) ----
    if (vk == VK_LWIN || vk == VK_RWIN) {
        if (down) {
            if (!winDown_) {
                winDown_ = true;
                winOtherKey_ = false;
                winComboInjected_ = false;
                winSwallow_ = IsDesktopForeground();
            }
            return winSwallow_;
        }
        // up
        if (!winDown_) return false;
        winDown_ = false;
        bool toggle = winSwallow_ && !winOtherKey_ && !winComboInjected_;
        bool swallow = winSwallow_ && !winComboInjected_;  // 组合键已注入 down, up 必须配对放行
        winSwallow_ = false;
        if (toggle && kOverlaysEnabled) ui_.ToggleTaskPanel();
        // 关闭态: 裸 Win 也放行 (Start 菜单照常), 用户只要壁纸动效
        if (!kOverlaysEnabled) swallow = false;
        return swallow;
    }

    // ---- Win 按住中再按其他键 -> 合成 Win-down, 保住 Win+D 等系统快捷键 ----
    if (winDown_ && winSwallow_ && !winComboInjected_ && down) {
        winOtherKey_ = true;
        winComboInjected_ = true;
        winSwallow_ = false;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (GetAsyncKeyState(VK_RWIN) & 0x8000) ? VK_RWIN : VK_LWIN;
        in.ki.dwFlags = 0;   // down
        SendInput(1, &in, sizeof(INPUT));
    }
    // Win+D -> 关闭全部覆盖层 (原 /wind 语义); 键本身放行给系统
    if (vk == 'D' && down && (winDown_ || winComboInjected_)) ui_.CloseAll();

    // ---- 覆盖层键盘路由 (编辑模式文本输入等), 仅桌面前台时转发 ----
    if (ui_.AnyVisible() && IsDesktopForeground()) {
        if (down) {
            wchar_t ch = 0;
            BYTE kb[256]{};
            GetKeyboardState(kb);
            kb[VK_SHIFT] = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x80 : 0;
            kb[VK_CONTROL] = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0x80 : 0;
            kb[VK_MENU] = (GetAsyncKeyState(VK_MENU) & 0x8000) ? 0x80 : 0;
            kb[VK_CAPITAL] = (GetAsyncKeyState(VK_CAPITAL) & 0x0001) ? 0x80 : 0;
            wchar_t buf[8]{};
            if (ToUnicodeEx(vk, info->scanCode, kb, buf, 8, 0, GetKeyboardLayout(0)) > 0)
                ch = buf[0];
            if (ui_.OnKey(vk, ch, true)) {
                vkSwallowed_[vk & 0xFF] = true;
                return true;
            }
        } else if (vkSwallowed_[vk & 0xFF]) {
            vkSwallowed_[vk & 0xFF] = false;
            return true;
        }
    }
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 默认定时器精度 15.625ms 会把 16ms 帧定时器对齐成 31.25ms (实测 ~31fps);
    // 提升到 1ms 后恢复 ~60fps (Win10 2004+ 按进程生效)。退出时成对释放。
    g_tbpRr = timeBeginPeriod(1);

    HANDLE mtx = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    wchar_t mod[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    std::wstring path(mod);
    // 日志名 = 可执行文件名 (Agent-Heart.exe -> Agent-Heart.log), 便于分发
    size_t slash = path.find_last_of(L'\\');
    std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    std::wstring leaf = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = leaf.find_last_of(L'.');
    if (dot != std::wstring::npos) leaf = leaf.substr(0, dot);
    g_logPath = dir + leaf + L".log";
    DeleteFileW(g_logPath.c_str());

    std::wstring cmd = GetCommandLineW();
    g_fullscreen = cmd.find(L"--windowed") == std::wstring::npos;
    g_probe = cmd.find(L"--probe") != std::wstring::npos || !g_fullscreen;

    // 自动化验收参数: --shot <path.png> / --open drawer|tasks / --interact hover|click|drag
    {
        std::vector<std::wstring> args;
        std::wstring cur;
        for (wchar_t c : cmd) {
            if (c == L'"') continue;   // 引号不入参 (start/bat 传参常带引号)
            if (c == L' ' || c == L'\t') {
                if (!cur.empty()) { args.push_back(cur); cur.clear(); }
            } else cur += c;
        }
        if (!cur.empty()) args.push_back(cur);
        {
            std::string joined;
            for (auto& a : args) joined += Narrow(a) + "|";
            Log("args: " + joined);
        }
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == L"--selftest-input") { g_selfTest = true; continue; }
            if (i + 1 >= args.size()) continue;   // 以下均为"带值"参数
            if (args[i] == L"--shot") g_shotPath = args[i + 1];
            else if (args[i] == L"--snapshot") g_snapPath = args[i + 1];
            else if (args[i] == L"--selfshot") g_selfShotPath = args[i + 1];
            else if (args[i] == L"--open") g_open = Narrow(args[i + 1]);
            else if (args[i] == L"--interact") g_interact = Narrow(args[i + 1]);
        }
        if (!g_shotPath.empty()) Log("shot mode: " + Narrow(g_shotPath));
        if (!g_selfShotPath.empty()) Log("selfshot mode: " + Narrow(g_selfShotPath));
        if (!g_snapPath.empty()) Log("snapshot mode: " + Narrow(g_snapPath));
        if (g_selfTest) { g_probe = true; Log("selftest-input mode ON (probe forced)"); }
        if (!g_open.empty()) Log("open trigger: " + g_open);
        if (!g_interact.empty()) Log("interact trigger: " + g_interact);
    }

    Log("=== Agent-Heart start ===");
    Log("timeBeginPeriod(1) rr=" + std::to_string(g_tbpRr) + " (0=ok)");
    Log(std::string("mode: ") + (g_fullscreen ? "wallpaper (WorkerW)" : "windowed") +
        (g_probe ? " [probe]" : ""));

    ConfigLoad();   // 启动加载用户配置 (%LOCALAPPDATA%\Agent-Heart\config.json)
    {
        const AppConfig& cfg = ConfigGet();
        char cb[160];
        sprintf_s(cb, "config: animations=%d scale=%.2f clickEffect=%d trail=%d",
                  cfg.animationsEnabled ? 1 : 0, static_cast<double>(cfg.sphereScale),
                  cfg.clickEffectEnabled ? 1 : 0, cfg.mouseTrailEnabled ? 1 : 0);
        Log(cb);
    }

    WallApp app;
    if (!app.Initialize(inst)) {
        Log("Initialize FAILED");
        return 1;
    }
    Log("message loop entered");
    return app.Run();
}
