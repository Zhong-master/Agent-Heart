// installer/setup.cpp —— Agent-Heart 零第三方安装向导(纯 Win32 C++20,深色现代主题)
// 安全约束:只写安装目标目录、HKCU Uninstall 键、shell:startup / 桌面 / 开始菜单快捷方式。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <system_error>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#ifndef DM_HASDEFID
#define DM_HASDEFID 0x0002
#endif

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
static const wchar_t* kProductName   = L"Agent-Heart";
static const wchar_t* kVersion       = L"1.0.0.0";
static const wchar_t* kExeName       = L"Agent-Heart.exe";
static const wchar_t* kDefaultDir    = L"%LOCALAPPDATA%\\Programs\\Agent-Heart";
static const wchar_t* kUninstallKey  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Agent-Heart";
static const wchar_t* kMutexName     = L"Local\\AgentHeartSetupSingleInstance";
static const wchar_t* kWndClass      = L"AgentHeartSetupWndClass";
static const wchar_t* kWndTitle      = L"Agent-Heart 安装程序";
// 内嵌 payload 资源表:id 与 payload.rc 固定对应;卸载清单与安装清单同源
struct PackFile { UINT resId; const wchar_t* name; };
static const PackFile kPackFiles[] = {
  { 101, L"Agent-Heart.exe" },
  { 102, L"LICENSE" },
  { 103, L"README.md" },
  { 104, L"CHANGELOG.md" },
};

// shell COM GUID 手写,避免依赖 uuid.lib(链接库清单保持任务规定)
static const GUID kCLSID_ShellLink  = {0x00021401,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID kIID_IShellLinkW   = {0x000214F9,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID kIID_IPersistFile  = {0x0000010B,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

enum : int {
  IDC_BACK    = 1001,
  IDC_NEXT    = 1002,
  IDC_CANCEL  = 1003,
  IDC_DIR_EDIT = 1010,
  IDC_BROWSE  = 1011,
  IDC_CHK_STARTUP = 1020,
  IDC_CHK_DESKTOP = 1021,
  IDC_CHK_MENU    = 1022,
  IDC_STATUS   = 1031,
  IDC_SUMMARY  = 1040,
  IDC_RUNNOW   = 1041,
};

#define WM_INSTALL_PROGRESS (WM_APP + 1)
#define WM_INSTALL_DONE     (WM_APP + 2)

// ---------------------------------------------------------------------------
// 深色主题配色
// ---------------------------------------------------------------------------
static inline COLORREF Hex(unsigned rgb) {
  return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
static const COLORREF kBg      = Hex(0x0E1116);   // 窗口底
static const COLORREF kCard    = Hex(0x161A21);   // 面板/卡片
static const COLORREF kText    = Hex(0xE8EAED);   // 主文字
static const COLORREF kText2   = RGB(0x96,0x98,0x9B); // 次级文字 ≈ white 55%
static const COLORREF kBorder  = RGB(0x35,0x38,0x3E); // 描边 ≈ white 14%
static const COLORREF kAcc1    = Hex(0x5B7CFA);   // 强调渐变起
static const COLORREF kAcc2    = Hex(0x8A5CF6);   // 强调渐变止
static const COLORREF kAccHot  = Hex(0x6E8BFF);   // 悬停
static const COLORREF kAccDown = Hex(0x4A63D6);   // 按下(更深)
static const COLORREF kHover   = Hex(0x232A36);   // 次级按钮悬停填充
static const COLORREF kTrack   = Hex(0x1E232C);   // 进度条轨道

static const int kClientW = 520;
static const int kClientH = 396;
static const int kBarY = 72;                      // 3px 强调渐变横条
static const int kCardL = 12, kCardT = 84, kCardR = 508, kCardB = 338;
static const RECT kEditRect  = { 32, 132, 408, 160 };
static const RECT kProgRect  = { 32, 146, 488, 162 };

static const int kPages = 5;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst = nullptr;
static HWND      g_hwnd = nullptr;
static HANDLE    g_mutex = nullptr;

static HFONT g_fontTitle = nullptr;  // Segoe UI 22 bold
static HFONT g_fontBlock = nullptr;  // 16 bold
static HFONT g_fontBody = nullptr;   // 13
static HFONT g_fontBtn = nullptr;    // 13
static HBRUSH g_bgBrush = nullptr;
static HBRUSH g_fieldBrush = nullptr;
static HICON  g_icon = nullptr;

static HWND g_title = nullptr;
static HWND g_sub = nullptr;
static HWND g_back = nullptr;
static HWND g_next = nullptr;
static HWND g_cancel = nullptr;

static HWND g_dirEdit = nullptr;
static HWND g_chkStartup = nullptr;
static HWND g_chkDesktop = nullptr;
static HWND g_chkMenu = nullptr;
static HWND g_status = nullptr;
static HWND g_summary = nullptr;
static HWND g_runNow = nullptr;

static std::vector<HWND> g_pageCtrl[kPages];
static std::unordered_set<HWND> g_secondary;      // 次级文字静态控件
static std::unordered_map<HWND, bool> g_hot;      // 悬停状态(自绘控件)

static int  g_page = 0;
static bool g_installing = false;
static bool g_installFailed = false;
static bool g_editFocus = false;
static bool g_chk[4] = { true, true, true, true };  // 自启/桌面/开始菜单/立即运行
static std::wstring g_installDir;
static std::wstring g_prefillDir;
static int  g_progPos = 0;
static int  g_progTotal = 0;
static DWORD g_sizeKB = 0;

static const wchar_t* kPageTitle[kPages] = {
  L"欢迎", L"选择安装位置", L"选择附加任务", L"正在安装", L"完成"
};
static const wchar_t* kPageSub[kPages] = {
  L"Agent-Heart 安装向导",
  L"选择 Agent-Heart 的安装目录",
  L"选择要执行的附加操作",
  L"请稍候,正在复制文件",
  L"Agent-Heart 已成功安装"
};

// ---------------------------------------------------------------------------
// 绘图小工具
// ---------------------------------------------------------------------------
static COLORREF MixColor(COLORREF a, COLORREF b, double t) {
  auto ch = [&](int x, int y) {
    int v = static_cast<int>(x + (y - x) * t + 0.5);
    return v < 0 ? 0 : (v > 255 ? 255 : v);
  };
  return RGB(ch(GetRValue(a), GetRValue(b)),
             ch(GetGValue(a), GetGValue(b)),
             ch(GetBValue(a), GetBValue(b)));
}
static COLORREF AlphaOver(COLORREF fg, COLORREF bg, double a) { return MixColor(bg, fg, a); }

static void GradientRect(HDC dc, RECT rc, COLORREF c1, COLORREF c2, bool vertical) {
  const int len = vertical ? (rc.bottom - rc.top) : (rc.right - rc.left);
  if (len <= 0) return;
  for (int i = 0; i < len; ++i) {
    const COLORREF c = (len == 1) ? c1 : MixColor(c1, c2, static_cast<double>(i) / (len - 1));
    HBRUSH b = CreateSolidBrush(c);
    RECT r = vertical ? RECT{ rc.left, rc.top + i, rc.right, rc.top + i + 1 }
                      : RECT{ rc.left + i, rc.top, rc.left + i + 1, rc.bottom };
    FillRect(dc, &r, b);
    DeleteObject(b);
  }
}

static void FillRound(HDC dc, RECT rc, int rad, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  HPEN p = CreatePen(PS_SOLID, 1, c);
  HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
  RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, rad * 2, rad * 2);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(b);
  DeleteObject(p);
}

static void BorderRound(HDC dc, RECT rc, int rad, COLORREF c, int w = 1) {
  HPEN p = CreatePen(PS_SOLID, w, c);
  HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
  HGDIOBJ op = SelectObject(dc, p);
  if (w <= 1)
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, rad * 2, rad * 2);
  else
    RoundRect(dc, rc.left + w / 2, rc.top + w / 2, rc.right - w / 2, rc.bottom - w / 2,
              rad * 2, rad * 2);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(p);
}

static void GradientRound(HDC dc, RECT rc, int rad, COLORREF c1, COLORREF c2, bool vertical) {
  if (rad <= 0) { GradientRect(dc, rc, c1, c2, vertical); return; }
  HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, rad * 2, rad * 2);
  const int saved = SaveDC(dc);
  SelectClipRgn(dc, rgn);
  GradientRect(dc, rc, c1, c2, vertical);
  RestoreDC(dc, saved);
  DeleteObject(rgn);
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::wstring GetModuleDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  PathRemoveFileSpecW(buf);
  return buf;
}

static std::wstring GetModulePath() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return buf;
}

static std::wstring ExpandEnv(const std::wstring& s) {
  wchar_t buf[1024] = {};
  DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 1024);
  return (n > 0 && n <= 1024) ? std::wstring(buf) : s;
}

static std::wstring Trim(const std::wstring& s) {
  size_t b = s.find_first_not_of(L" \t\"");
  if (b == std::wstring::npos) return L"";
  size_t e = s.find_last_not_of(L" \t\"");
  return s.substr(b, e - b + 1);
}

static std::wstring GetCsidlFolder(int csidl) {
  wchar_t buf[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, buf)))
    SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf);
  return buf;
}

static void StripTrailingSlash(std::wstring& s) {
  while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
}

static bool AttachOrAllocConsole() {
  if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return false;
  FILE* f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  return true;
}

// ---------------------------------------------------------------------------
// 快捷方式 / 注册表(安装逻辑,勿动)
// ---------------------------------------------------------------------------
static bool CreateShortcut(const std::wstring& linkPath, const std::wstring& target,
                           const std::wstring& workDir) {
  wchar_t parentBuf[MAX_PATH] = {};
  wcsncpy_s(parentBuf, linkPath.c_str(), _TRUNCATE);
  PathRemoveFileSpecW(parentBuf);
  if (parentBuf[0] && GetFileAttributesW(parentBuf) == INVALID_FILE_ATTRIBUTES)
    CreateDirectoryW(parentBuf, nullptr);

  IShellLinkW* psl = nullptr;
  HRESULT hr = CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_IShellLinkW, reinterpret_cast<void**>(&psl));
  if (FAILED(hr) || !psl) return false;

  psl->SetPath(target.c_str());
  psl->SetWorkingDirectory(workDir.c_str());
  psl->SetIconLocation(target.c_str(), 0);
  psl->SetShowCmd(SW_SHOWNORMAL);

  bool ok = false;
  IPersistFile* ppf = nullptr;
  if (SUCCEEDED(psl->QueryInterface(kIID_IPersistFile, reinterpret_cast<void**>(&ppf))) && ppf) {
    ok = SUCCEEDED(ppf->Save(linkPath.c_str(), TRUE));
    ppf->Release();
  }
  psl->Release();
  return ok;
}

static bool WriteUninstallKey(const std::wstring& dir, const std::wstring& setupExe, DWORD sizeKB) {
  HKEY key = nullptr;
  DWORD disp = 0;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE,
                      nullptr, &key, &disp) != ERROR_SUCCESS)
    return false;

  auto setSz = [&](const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  };
  setSz(L"DisplayName", kProductName);
  setSz(L"DisplayVersion", kVersion);
  setSz(L"DisplayIcon", dir + L"\\" + kExeName);
  setSz(L"Publisher", kProductName);
  setSz(L"InstallLocation", dir);
  setSz(L"UninstallString", L"\"" + setupExe + L"\" --uninstall");
  setSz(L"QuietUninstallString", L"\"" + setupExe + L"\" --uninstall");

  DWORD kb = sizeKB;
  RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&kb), sizeof(kb));
  DWORD one = 1;
  RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
  RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
  RegCloseKey(key);
  return true;
}

// ---------------------------------------------------------------------------
// 安装主体(向导线程与 --silent 都走这里)
// ---------------------------------------------------------------------------
struct ProgMsg { std::wstring file; int done = 0; int total = 0; };

static DWORD InstallBody(const std::wstring& dir, bool optStartup, bool optDesktop, bool optMenu,
                         bool gui, std::wstring& err) {
  const std::wstring payload = GetModuleDir() + L"\\payload";

  // 安装清单:优先读自身 RCDATA 资源(单文件分发),资源缺失回退 payload 目录(开发期兼容)
  struct Item { const wchar_t* name; UINT id; const void* data; DWORD size; std::wstring path; bool fromRes; };
  std::vector<Item> list;
  std::wstring missing;
  for (const auto& pf : kPackFiles) {
    Item it{ pf.name, pf.resId, nullptr, 0, std::wstring(), false };
    HRSRC hr = FindResourceW(g_hInst, MAKEINTRESOURCEW(pf.resId), RT_RCDATA);
    if (hr) {
      HGLOBAL hg = LoadResource(g_hInst, hr);
      const void* p = hg ? LockResource(hg) : nullptr;
      const DWORD sz = SizeofResource(g_hInst, hr);
      if (p && sz) {
        it.data = p;
        it.size = sz;
        it.fromRes = true;
        list.push_back(it);
        continue;
      }
    }
    const std::wstring disk = payload + L"\\" + pf.name;
    const DWORD attr = GetFileAttributesW(disk.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      it.path = disk;
      list.push_back(it);
      continue;
    }
    missing += std::wstring(L"  - ") + pf.name + L" (内嵌资源 id=" + std::to_wstring(pf.resId) +
               L" 缺失,且 payload 目录无副本)\n";
  }
  if (!missing.empty()) {
    err = L"以下文件既无内嵌资源也无 payload 目录副本:\n" + missing +
          L"payload 目录: " + payload;
    return 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"无法创建安装目录:\n" + dir;
    return 2;
  }

  unsigned long long totalBytes = 0;
  int done = 0;
  const int total = static_cast<int>(list.size());
  for (const auto& s : list) {
    const std::wstring dst = dir + L"\\" + s.name;
    if (s.fromRes) {
      // 内嵌资源 → 直接写目标文件
      HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) {
        err = L"内嵌资源写入失败:" + std::wstring(s.name) + L"\n目标:" + dst +
              L"\n系统错误代码:" + std::to_wstring(GetLastError());
        return 3;
      }
      DWORD written = 0;
      const BOOL wr = WriteFile(f, s.data, s.size, &written, nullptr);
      const DWORD we = (wr && written == s.size) ? 0 : GetLastError();
      CloseHandle(f);
      if (!wr || written != s.size) {
        err = L"内嵌资源写入不完整:" + std::wstring(s.name) + L"\n目标:" + dst +
              L"\n系统错误代码:" + std::to_wstring(we);
        return 3;
      }
      totalBytes += s.size;
    } else {
      // 回退:payload 目录复制
      if (!CopyFileW(s.path.c_str(), dst.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        err = L"payload 目录复制失败:" + std::wstring(s.name) + L"\n来源:" + s.path +
              L"\n目标:" + dst + L"\n系统错误代码:" + std::to_wstring(e);
        return 3;
      }
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (GetFileAttributesExW(s.path.c_str(), GetFileExInfoStandard, &fad))
        totalBytes += (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    ++done;
    if (gui && g_hwnd) {
      auto* pm = new ProgMsg{std::wstring(s.name), done, total};
      if (!SendMessageW(g_hwnd, WM_INSTALL_PROGRESS, 0, reinterpret_cast<LPARAM>(pm))) delete pm;
    } else {
      printf("[install] %d/%d %ls [%ls]\n", done, total, s.name,
             s.fromRes ? L"内嵌资源" : L"payload目录");
    }
  }

  const std::wstring target = dir + L"\\" + kExeName;
  if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"安装后未找到主程序(请确认 payload 中包含 Agent-Heart.exe):\n" + target;
    return 4;
  }

  if (optDesktop) {
    const std::wstring desk = GetCsidlFolder(CSIDL_DESKTOPDIRECTORY);
    if (desk.empty() || !CreateShortcut(desk + L"\\Agent-Heart.lnk", target, dir)) {
      err = L"创建桌面快捷方式失败。";
      return 5;
    }
  }
  if (optMenu) {
    const std::wstring programs = GetCsidlFolder(CSIDL_PROGRAMS);
    const std::wstring folder = programs + L"\\Agent-Heart";
    if (programs.empty() || !CreateShortcut(folder + L"\\Agent-Heart.lnk", target, dir)) {
      err = L"创建开始菜单快捷方式失败。";
      return 6;
    }
  }
  if (optStartup) {
    const std::wstring startup = GetCsidlFolder(CSIDL_STARTUP);
    if (startup.empty() || !CreateShortcut(startup + L"\\Agent-Heart.lnk", target, dir)) {
      err = L"创建开机自启快捷方式失败。";
      return 7;
    }
  }

  g_sizeKB = static_cast<DWORD>((totalBytes + 1023) / 1024);
  if (!WriteUninstallKey(dir, GetModulePath(), g_sizeKB)) {
    err = L"写入 HKCU 卸载注册表项失败。";
    return 8;
  }
  return 0;
}

static DWORD RunInstall(const std::wstring& dir, bool optStartup, bool optDesktop, bool optMenu,
                        bool gui, std::wstring& err) {
  HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  DWORD rc = InstallBody(dir, optStartup, optDesktop, optMenu, gui, err);
  if (SUCCEEDED(hrCom)) CoUninitialize();
  if (gui && g_hwnd) {
    auto* em = new std::wstring(err);
    if (!SendMessageW(g_hwnd, WM_INSTALL_DONE, rc == 0 ? 1 : 0, reinterpret_cast<LPARAM>(em))) delete em;
  }
  return rc;
}

// ---------------------------------------------------------------------------
// 静默卸载 --uninstall
// ---------------------------------------------------------------------------
static int RunUninstall() {
  AttachOrAllocConsole();

  std::wstring dir;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ && size >= sizeof(wchar_t)) {
      std::wstring buf(size / sizeof(wchar_t), L'\0');
      DWORD sz = size;
      if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                           reinterpret_cast<BYTE*>(&buf[0]), &sz) == ERROR_SUCCESS) {
        buf.resize(wcslen(buf.c_str()));
        dir = Trim(buf);
      }
    }
    RegCloseKey(key);
  }
  if (dir.empty()) {
    printf("[uninstall] 错误:未找到卸载注册表项或 InstallLocation 为空,无法确定安装目录。\n");
    return 1;
  }
  printf("[uninstall] 安装目录: %ls\n", dir.c_str());

  int failed = 0;
  for (const auto& pf : kPackFiles) {
    const wchar_t* f = pf.name;
    const std::wstring p = dir + L"\\" + f;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
      printf("[uninstall] 跳过(不存在): %ls\n", f);
      continue;
    }
    if (DeleteFileW(p.c_str())) printf("[uninstall] 已删除文件: %ls\n", f);
    else { printf("[uninstall] 删除失败: %ls (错误 %lu)\n", f, GetLastError()); ++failed; }
  }

  if (RemoveDirectoryW(dir.c_str())) printf("[uninstall] 已删除安装目录: %ls\n", dir.c_str());
  else if (GetLastError() == ERROR_DIR_NOT_EMPTY)
    printf("[uninstall] 安装目录非空,保留: %ls\n", dir.c_str());
  else
    printf("[uninstall] 安装目录不存在或无法删除: %ls (错误 %lu)\n", dir.c_str(), GetLastError());

  struct Lnk { const wchar_t* label; std::wstring path; };
  std::vector<Lnk> links;
  const std::wstring desk = GetCsidlFolder(CSIDL_DESKTOPDIRECTORY);
  if (!desk.empty()) links.push_back({L"桌面快捷方式", desk + L"\\Agent-Heart.lnk"});
  const std::wstring startup = GetCsidlFolder(CSIDL_STARTUP);
  if (!startup.empty()) links.push_back({L"开机自启快捷方式", startup + L"\\Agent-Heart.lnk"});
  const std::wstring programs = GetCsidlFolder(CSIDL_PROGRAMS);
  if (!programs.empty()) links.push_back({L"开始菜单快捷方式", programs + L"\\Agent-Heart\\Agent-Heart.lnk"});
  for (const auto& l : links) {
    if (GetFileAttributesW(l.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      printf("[uninstall] 跳过(不存在): %ls\n", l.label);
      continue;
    }
    if (DeleteFileW(l.path.c_str())) printf("[uninstall] 已删除: %ls\n", l.label);
    else { printf("[uninstall] 删除失败: %ls (错误 %lu)\n", l.label, GetLastError()); ++failed; }
  }
  if (!programs.empty()) {
    const std::wstring folder = programs + L"\\Agent-Heart";
    if (RemoveDirectoryW(folder.c_str())) printf("[uninstall] 已删除开始菜单程序文件夹\n");
    else printf("[uninstall] 开始菜单程序文件夹不存在或非空\n");
  }

  LONG st = RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
  if (st == ERROR_SUCCESS) printf("[uninstall] 已删除 HKCU 卸载注册表项\n");
  else printf("[uninstall] 卸载注册表项删除失败/不存在 (错误 %ld)\n", st);

  printf("[uninstall] 完成%s\n", failed ? " (存在失败项)" : "");
  return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 向导界面(深色自绘)
// ---------------------------------------------------------------------------
static bool IsHot(HWND h) {
  auto it = g_hot.find(h);
  return it != g_hot.end() && it->second;
}

static bool IsSecondary(HWND ctl) { return ctl == g_sub || g_secondary.count(ctl) > 0; }

// 悬停跟踪子类:WM_MOUSEMOVE 点亮、WM_MOUSELEAVE 熄灭
static bool IsChkId(UINT_PTR id) {
  return id == IDC_CHK_STARTUP || id == IDC_CHK_DESKTOP ||
         id == IDC_CHK_MENU || id == IDC_RUNNOW;
}

static void ToggleChk(UINT_PTR id) {
  if (id == IDC_CHK_STARTUP) g_chk[0] = !g_chk[0];
  else if (id == IDC_CHK_DESKTOP) g_chk[1] = !g_chk[1];
  else if (id == IDC_CHK_MENU) g_chk[2] = !g_chk[2];
  else if (id == IDC_RUNNOW) g_chk[3] = !g_chk[3];
}

static LRESULT CALLBACK HotSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR) {
  switch (msg) {
    case WM_LBUTTONUP:
      if (IsChkId(id)) { ToggleChk(id); InvalidateRect(h, nullptr, TRUE); }
      break;
    case WM_KEYDOWN:
      if (wp == VK_SPACE && IsChkId(id)) {
        ToggleChk(id);
        InvalidateRect(h, nullptr, TRUE);
        return 0;
      }
      break;
    case WM_MOUSEMOVE:
      if (!IsHot(h)) {
        g_hot[h] = true;
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      g_hot[h] = false;
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;  // 背景由父窗口/自绘覆盖,避免闪烁
  }
  return DefSubclassProc(h, msg, wp, lp);
}

static HWND MakeCtrl(int page, DWORD exStyle, const wchar_t* cls, const wchar_t* text,
                     DWORD style, int x, int y, int w, int cy, int id, bool secondary = false) {
  HWND ctrl = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_TABSTOP | style,
                              x, y, w, cy, g_hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              g_hInst, nullptr);
  if (ctrl) {
    SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    g_pageCtrl[page].push_back(ctrl);
    if (secondary) g_secondary.insert(ctrl);
  }
  return ctrl;
}

static void UpdatePage() {
  for (int p = 0; p < kPages; ++p)
    for (HWND c : g_pageCtrl[p]) ShowWindow(c, SW_HIDE);
  for (HWND c : g_pageCtrl[g_page]) ShowWindow(c, SW_SHOW);

  SetWindowTextW(g_title, kPageTitle[g_page]);
  SetWindowTextW(g_sub, kPageSub[g_page]);

  // 标题/副标题/底部按钮不在任何一页的列表里,统一显示
  ShowWindow(g_title, SW_SHOW);
  ShowWindow(g_sub, SW_SHOW);
  ShowWindow(g_next, SW_SHOW);
  ShowWindow(g_cancel, SW_SHOW);
  ShowWindow(g_back, (g_page == 0 || g_page == 3 || g_page == 4) ? SW_HIDE : SW_SHOW);
  EnableWindow(g_cancel, g_installing ? FALSE : TRUE);

  switch (g_page) {
    case 0:
      SetWindowTextW(g_next, L"下一步(&N) >");
      EnableWindow(g_next, TRUE);
      break;
    case 1:
    case 2:
      SetWindowTextW(g_back, L"< 上一步(&B)");
      SetWindowTextW(g_next, g_page == 1 ? L"下一步(&N) >" : L"安装(&I)");
      EnableWindow(g_next, TRUE);
      break;
    case 3:
      if (g_installing) {
        SetWindowTextW(g_next, L"正在安装…");
        EnableWindow(g_next, FALSE);
      } else if (g_installFailed) {
        SetWindowTextW(g_next, L"重试(&R)");
        EnableWindow(g_next, TRUE);
      } else {
        SetWindowTextW(g_next, L"安装(&I)");
        EnableWindow(g_next, TRUE);
      }
      break;
    case 4:
      SetWindowTextW(g_next, L"完成(&F)");
      EnableWindow(g_next, TRUE);
      break;
  }
  g_hot.clear();  // 翻页后清除悬停状态
  InvalidateRect(g_hwnd, nullptr, TRUE);
  if (IsWindowVisible(g_next)) SetFocus(g_next);
}

static void BrowseFolder(HWND owner, HWND edit) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"选择 Agent-Heart 的安装位置";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (pidl) {
    wchar_t path[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(edit, path);
    CoTaskMemFree(pidl);
  }
}

struct InstallJob { std::wstring dir; bool startup = false; bool desktop = false; bool menu = false; };

static DWORD WINAPI InstallThreadProc(LPVOID p) {
  std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(p));
  std::wstring err;
  RunInstall(job->dir, job->startup, job->desktop, job->menu, true, err);
  return 0;
}

static void StartInstall(HWND hwnd) {
  g_progPos = 0;
  g_progTotal = 0;
  SetWindowTextW(g_status, L"正在准备…");
  RECT pr = kProgRect;
  InvalidateRect(hwnd, &pr, FALSE);
  auto* job = new InstallJob{g_installDir, g_chk[0], g_chk[1], g_chk[2]};
  HANDLE th = CreateThread(nullptr, 0, InstallThreadProc, job, 0, nullptr);
  if (!th) {
    delete job;
    g_installing = false;
    g_installFailed = true;
    UpdatePage();
    MessageBoxW(hwnd, L"无法创建安装线程。", L"安装失败", MB_ICONERROR | MB_OK);
    return;
  }
  CloseHandle(th);
}

static void OnNext(HWND hwnd) {
  switch (g_page) {
    case 0:
      g_page = 1;
      UpdatePage();
      break;
    case 1: {
      wchar_t buf[1024] = {};
      GetWindowTextW(g_dirEdit, buf, 1024);
      std::wstring dir = Trim(ExpandEnv(Trim(buf)));
      if (dir.empty()) {
        MessageBoxW(hwnd, L"请输入安装位置。", kWndTitle, MB_ICONWARNING | MB_OK);
        SetFocus(g_dirEdit);
        return;
      }
      if (PathIsRelativeW(dir.c_str())) {
        MessageBoxW(hwnd, L"请使用绝对路径,例如 C:\\Users\\你\\AppData\\Local\\Programs\\Agent-Heart",
                    kWndTitle, MB_ICONWARNING | MB_OK);
        SetFocus(g_dirEdit);
        return;
      }
      StripTrailingSlash(dir);
      g_installDir = dir;
      g_page = 2;
      UpdatePage();
      break;
    }
    case 2:
      g_page = 3;
      g_installing = true;
      g_installFailed = false;
      UpdatePage();
      StartInstall(hwnd);
      break;
    case 3:
      break;
    case 4: {
      if (g_chk[3]) {
        const std::wstring exe = g_installDir + L"\\" + kExeName;
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
          HINSTANCE hi = ShellExecuteW(hwnd, L"open", exe.c_str(), nullptr, g_installDir.c_str(),
                                       SW_SHOWNORMAL);
          if (reinterpret_cast<INT_PTR>(hi) <= 32)
            MessageBoxW(hwnd, L"无法启动 Agent-Heart.exe,可稍后手动运行。", kWndTitle,
                        MB_ICONWARNING | MB_OK);
        }
      }

      DestroyWindow(hwnd);
      break;
    }
  }
}

// ---- 自绘按钮 / 复选框 ----
static LRESULT OnDrawItem(DRAWITEMSTRUCT* ds) {
  if (ds->CtlType != ODT_BUTTON) return FALSE;
  HWND h = ds->hwndItem;
  const UINT id = ds->CtlID;
  const bool disabled = (ds->itemState & ODS_DISABLED) != 0;
  const bool pressed  = (ds->itemState & ODS_SELECTED) != 0;
  const bool focus    = (ds->itemState & ODS_FOCUS) != 0;
  const bool checked  = (id == IDC_CHK_STARTUP) ? g_chk[0] :
                        (id == IDC_CHK_DESKTOP) ? g_chk[1] :
                        (id == IDC_CHK_MENU)    ? g_chk[2] : g_chk[3];
  const bool hot      = IsHot(h);
  HDC dc = ds->hDC;
  RECT rc = ds->rcItem;
  SetBkMode(dc, TRANSPARENT);

  HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
  HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

  wchar_t text[512] = {};
  GetWindowTextW(h, text, 512);

  if (id == IDC_CHK_STARTUP || id == IDC_CHK_DESKTOP || id == IDC_CHK_MENU ||
      id == IDC_RUNNOW) {
    // --- 自绘复选框:强调色对勾 + 悬停 ---
    const int box = 18;
    const int bx = rc.left;
    const int by = rc.top + ((rc.bottom - rc.top) - box) / 2;
    RECT br{ bx, by, bx + box, by + box };
    COLORREF border = checked ? kAcc1 : (hot ? kAcc1 : kBorder);
    COLORREF fill = checked ? kAcc1 : (hot ? kHover : kBg);
    if (disabled) { fill = AlphaOver(fill, kBg, 0.4); border = AlphaOver(border, kBg, 0.4); }
    FillRound(dc, br, 5, fill);
    BorderRound(dc, br, 5, border);
    if (checked && !disabled) {
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
      HGDIOBJ op = SelectObject(dc, pen);
      HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
      MoveToEx(dc, bx + 4, by + 9, nullptr);
      LineTo(dc, bx + 8, by + 13);
      LineTo(dc, bx + 14, by + 5);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(pen);
    } else if (checked && disabled) {
      HPEN pen = CreatePen(PS_SOLID, 2, AlphaOver(kText, kBg, 0.4));
      HGDIOBJ op = SelectObject(dc, pen);
      HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
      MoveToEx(dc, bx + 4, by + 9, nullptr);
      LineTo(dc, bx + 8, by + 13);
      LineTo(dc, bx + 14, by + 5);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(pen);
    }
    SetTextColor(dc, disabled ? AlphaOver(kText, kBg, 0.4) : kText);
    RECT tr{ bx + box + 10, rc.top, rc.right, rc.bottom };
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
    return TRUE;
  }

  // --- 自绘圆角按钮 ---
  const bool primary = (id == IDC_NEXT);
  if (primary) {
    COLORREF c1 = kAcc1, c2 = kAcc2;
    if (pressed) { c1 = kAccDown; c2 = MixColor(kAcc2, kBg, 0.35); }
    else if (hot) { c1 = kAccHot; c2 = MixColor(kAcc2, kAccHot, 0.35); }
    if (disabled) { c1 = AlphaOver(c1, kBg, 0.4); c2 = AlphaOver(c2, kBg, 0.4); }
    GradientRound(dc, rc, 8, c1, c2, true);
    BorderRound(dc, rc, 8, disabled ? AlphaOver(kBorder, kBg, 0.4) : MixColor(kAcc2, kBg, 0.35));
    SetTextColor(dc, disabled ? AlphaOver(kText, kBg, 0.4) : RGB(255, 255, 255));
  } else {
    COLORREF fill = kCard, border = kBorder;
    if (hot) { fill = kHover; border = kAcc1; }
    if (pressed) { fill = MixColor(kHover, kBg, 0.4); border = kAcc1; }
    if (disabled) { fill = AlphaOver(fill, kBg, 0.4); border = AlphaOver(border, kBg, 0.4); }
    FillRound(dc, rc, 8, fill);
    BorderRound(dc, rc, 8, border);
    if (focus && !disabled) BorderRound(dc, RECT{ rc.left - 2, rc.top - 2, rc.right + 2, rc.bottom + 2 },
                                        9, AlphaOver(kAcc1, kBg, 0.55));
    SetTextColor(dc, disabled ? AlphaOver(kText, kBg, 0.4) : kText);
  }
  DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (oldFont) SelectObject(dc, oldFont);
  return TRUE;
}

static void OnCreate(HWND hwnd) {
  g_fontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_fontBlock = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_fontBody = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_fontBtn = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_bgBrush = CreateSolidBrush(kBg);
  g_fieldBrush = CreateSolidBrush(kBg);
  g_icon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

  // 头部:标题旁小图标 + 22pt 粗标题 + 副标题
  g_title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 54, 8, 440, 34, hwnd, nullptr, g_hInst, nullptr);
  SendMessageW(g_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontTitle), TRUE);
  g_sub = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 54, 44, 440, 20, hwnd, nullptr, g_hInst, nullptr);
  SendMessageW(g_sub, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
  g_secondary.insert(g_sub);

  // 页 0:欢迎(左侧 48px 大图标在 WM_PAINT 里画)
  HWND h = MakeCtrl(0, 0, L"STATIC", L"欢迎使用 Agent-Heart 安装向导", WS_CHILD,
                    96, 118, 392, 30, 0);
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBlock), TRUE);
  MakeCtrl(0, 0, L"STATIC",
           L"Agent-Heart 是纯原生 Win32 动效壁纸程序。\n"
           L"本向导将把程序文件复制到你的电脑,并按你的选择创建快捷方式。",
           WS_CHILD, 32, 178, 456, 54, 0, true);
  MakeCtrl(0, 0, L"STATIC",
           L"产品版本:1.0.0.0\n"
           L"建议在继续前退出正在运行的 Agent-Heart。\n"
           L"点击“下一步”继续。",
           WS_CHILD, 32, 240, 456, 88, 0, true);

  // 页 1:安装位置
  MakeCtrl(1, 0, L"STATIC", L"安装位置:", WS_CHILD, 32, 106, 456, 20, 0);
  g_dirEdit = MakeCtrl(1, 0, L"EDIT",
                       g_prefillDir.empty() ? kDefaultDir : g_prefillDir.c_str(),
                       WS_CHILD | ES_AUTOHSCROLL, 32, 132, 376, 28, IDC_DIR_EDIT);
  if (g_dirEdit)
    SendMessageW(g_dirEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
  MakeCtrl(1, 0, L"BUTTON", L"浏览(&B)…", WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
           416, 132, 72, 28, IDC_BROWSE);
  MakeCtrl(1, 0, L"STATIC",
           L"程序文件将复制到该目录,默认是 %LOCALAPPDATA%\\Programs\\Agent-Heart。\n"
           L"该目录位于当前用户配置下,无需管理员权限。",
           WS_CHILD, 32, 172, 456, 60, 0, true);

  // 页 2:选项
  MakeCtrl(2, 0, L"STATIC", L"选择附加任务:", WS_CHILD, 32, 106, 456, 20, 0);
  g_chkStartup = MakeCtrl(2, 0, L"BUTTON",
                          L"开机自启(在启动文件夹创建 Agent-Heart.lnk)",
                          WS_CHILD | BS_OWNERDRAW | BS_LEFT,
                          32, 138, 456, 24, IDC_CHK_STARTUP);
  g_chkDesktop = MakeCtrl(2, 0, L"BUTTON", L"创建桌面快捷方式",
                          WS_CHILD | BS_OWNERDRAW | BS_LEFT,
                          32, 170, 456, 24, IDC_CHK_DESKTOP);
  g_chkMenu = MakeCtrl(2, 0, L"BUTTON", L"创建开始菜单快捷方式(程序文件夹)",
                       WS_CHILD | BS_OWNERDRAW | BS_LEFT,
                       32, 202, 456, 24, IDC_CHK_MENU);
  MakeCtrl(2, 0, L"STATIC", L"所有快捷方式都指向安装目录中的 Agent-Heart.exe。",
           WS_CHILD, 32, 240, 456, 50, 0, true);

  // 页 3:安装(进度条由父窗口自绘,不放控件)
  MakeCtrl(3, 0, L"STATIC", L"正在安装,请稍候……", WS_CHILD, 32, 110, 456, 20, 0);
  g_status = MakeCtrl(3, 0, L"STATIC", L"准备中…", WS_CHILD | SS_PATHELLIPSIS,
                      32, 178, 456, 70, IDC_STATUS, true);

  // 页 4:完成
  h = MakeCtrl(4, 0, L"STATIC", L"安装完成。", WS_CHILD, 32, 110, 456, 30, 0);
  SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBlock), TRUE);
  g_summary = MakeCtrl(4, 0, L"STATIC", L"", WS_CHILD, 32, 152, 456, 56, IDC_SUMMARY);
  g_runNow = MakeCtrl(4, 0, L"BUTTON", L"立即运行 Agent-Heart.exe",
                      WS_CHILD | BS_OWNERDRAW | BS_LEFT,
                      32, 222, 456, 24, IDC_RUNNOW);
  MakeCtrl(4, 0, L"STATIC", L"点击“完成”退出安装向导。", WS_CHILD, 32, 262, 456, 40, 0, true);

  // 底部按钮(自绘圆角)
  g_back = CreateWindowExW(0, L"BUTTON", L"< 上一步(&B)",
                           WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                           216, 352, 84, 32, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BACK)), g_hInst, nullptr);
  g_next = CreateWindowExW(0, L"BUTTON", L"下一步(&N) >",
                           WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                           312, 352, 84, 32, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEXT)), g_hInst, nullptr);
  g_cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                             WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                             404, 352, 84, 32, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL)), g_hInst, nullptr);
  SendMessageW(g_back, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBtn), TRUE);
  SendMessageW(g_next, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBtn), TRUE);
  SendMessageW(g_cancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBtn), TRUE);

  // 悬停跟踪
  SetWindowSubclass(g_back, HotSubclass, IDC_BACK, 0);
  SetWindowSubclass(g_next, HotSubclass, IDC_NEXT, 0);
  SetWindowSubclass(g_cancel, HotSubclass, IDC_CANCEL, 0);
  SetWindowSubclass(g_dirEdit, HotSubclass, IDC_DIR_EDIT, 0);
  HWND browse = GetDlgItem(hwnd, IDC_BROWSE);
  if (browse) SetWindowSubclass(browse, HotSubclass, IDC_BROWSE, 0);
  for (HWND cb : { g_chkStartup, g_chkDesktop, g_chkMenu, g_runNow })
    if (cb) SetWindowSubclass(cb, HotSubclass, static_cast<UINT_PTR>(GetDlgCtrlID(cb)), 0);

  SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));

  UpdatePage();
}

static void OnPaint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);

  RECT cl;
  GetClientRect(hwnd, &cl);
  FillRect(dc, &cl, g_bgBrush ? g_bgBrush : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  // 标题旁小图标(28px)
  if (g_icon) DrawIconEx(dc, 18, 13, g_icon, 28, 28, 0, nullptr, DI_NORMAL);

  // 头部 3px 强调渐变横条
  RECT bar{ 0, kBarY, kClientW, kBarY + 3 };
  GradientRect(dc, bar, kAcc1, kAcc2, false);

  // 内容卡片
  RECT card{ kCardL, kCardT, kCardR, kCardB };
  FillRound(dc, card, 12, kCard);
  BorderRound(dc, card, 12, kBorder);

  // 欢迎页大图标 48px
  if (g_page == 0 && g_icon) DrawIconEx(dc, 36, 118, g_icon, 48, 48, 0, nullptr, DI_NORMAL);

  // 安装位置输入框描边(控件本身无边框,自绘)
  if (g_page == 1) {
    RECT f = kEditRect;
    FillRound(dc, f, 6, kBg);
    BorderRound(dc, f, 6, g_editFocus ? kAcc1 : kBorder);
  }

  // 自绘进度条:圆角轨道 + 强调渐变填充
  if (g_page == 3) {
    RECT track = kProgRect;
    FillRound(dc, track, 8, kTrack);
    BorderRound(dc, track, 8, kBorder);
    if (g_progTotal > 0 && g_progPos > 0) {
      int w = static_cast<int>((static_cast<long long>(kProgRect.right - kProgRect.left) *
                                g_progPos) / g_progTotal);
      const int full = kProgRect.right - kProgRect.left;
      if (w > full) w = full;
      if (w < 16) w = 16;
      RECT fill{ kProgRect.left, kProgRect.top, kProgRect.left + w, kProgRect.bottom };
      GradientRound(dc, fill, 8, kAcc1, kAcc2, false);
      BorderRound(dc, fill, 8, AlphaOver(kAcc2, kBg, 0.5));
    }
  }

}

static LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_hwnd = hwnd;
      OnCreate(hwnd);
      return 0;

    case WM_PAINT:
      OnPaint(hwnd);
      return 0;

    case WM_ERASEBKGND:
      return 1;  // 背景在 WM_PAINT 里整幅绘制

    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wp);
      if (!g_fieldBrush) g_fieldBrush = CreateSolidBrush(kBg);
      SetBkColor(dc, kBg);
      SetTextColor(dc, kText);
      return reinterpret_cast<LRESULT>(g_fieldBrush);
    }

    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      HWND ctl = reinterpret_cast<HWND>(lp);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, IsSecondary(ctl) ? kText2 : kText);
      return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }

    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }

    case WM_DRAWITEM:
      return OnDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_BROWSE:
          BrowseFolder(hwnd, g_dirEdit);
          break;
        case IDC_BACK:
          if (!g_installing && g_page > 0) { --g_page; UpdatePage(); }
          break;
        case IDC_NEXT:
          OnNext(hwnd);
          break;
        case IDC_CANCEL:
        case IDCANCEL:
          if (g_installing) { MessageBeep(MB_ICONWARNING); break; }
          DestroyWindow(hwnd);
          break;
      }
      if (LOWORD(wp) == IDC_DIR_EDIT) {
        if (HIWORD(wp) == EN_SETFOCUS) { g_editFocus = true; RECT r = kEditRect; InvalidateRect(hwnd, &r, TRUE); }
        else if (HIWORD(wp) == EN_KILLFOCUS) { g_editFocus = false; RECT r = kEditRect; InvalidateRect(hwnd, &r, TRUE); }
      }
      return 0;

    case WM_INSTALL_PROGRESS: {
      std::unique_ptr<ProgMsg> pm(reinterpret_cast<ProgMsg*>(lp));
      if (pm && pm->total > 0) {
        g_progPos = pm->done;
        g_progTotal = pm->total;
        wchar_t buf[1024];
        swprintf_s(buf, L"已复制 %d/%d 个文件:\n%s", pm->done, pm->total, pm->file.c_str());
        SetWindowTextW(g_status, buf);
        RECT pr = kProgRect;
        InvalidateRect(hwnd, &pr, FALSE);
      }
      return 1;
    }

    case WM_INSTALL_DONE: {
      std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
      g_installing = false;
      if (wp) {
        g_installFailed = false;
        g_page = 4;
        g_progPos = g_progTotal > 0 ? g_progTotal : g_progPos;
        const std::wstring sum = std::wstring(L"Agent-Heart 已安装到:\n") + g_installDir;
        SetWindowTextW(g_summary, sum.c_str());
        UpdatePage();
      } else {
        g_installFailed = true;
        UpdatePage();
        MessageBoxW(hwnd, (err && !err->empty()) ? err->c_str() : L"安装失败,未知原因。",
                    L"安装失败", MB_ICONERROR | MB_OK);
      }
      return 1;
    }

    case WM_CLOSE:
      if (g_installing) { MessageBeep(MB_ICONWARNING); return 0; }
      return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_DESTROY:
      if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
      if (g_fontBlock) { DeleteObject(g_fontBlock); g_fontBlock = nullptr; }
      if (g_fontBody) { DeleteObject(g_fontBody); g_fontBody = nullptr; }
      if (g_fontBtn) { DeleteObject(g_fontBtn); g_fontBtn = nullptr; }
      if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
      if (g_fieldBrush) { DeleteObject(g_fieldBrush); g_fieldBrush = nullptr; }
      g_hot.clear();
      PostQuitMessage(0);
      return 0;

    case DM_GETDEFID:
      return MAKELONG(IDC_NEXT, DM_HASDEFID);

    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
enum class Mode { Gui, Silent, Uninstall };

static int RunGui(int showCmd) {
  if (!g_bgBrush) g_bgBrush = CreateSolidBrush(kBg);
  WNDCLASSEXW wc{ sizeof(wc) };
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = SetupWndProc;
  wc.hInstance = g_hInst;
  wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = g_bgBrush;
  wc.lpszClassName = kWndClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

  INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
  InitCommonControlsEx(&icc);

  RECT rc{ 0, 0, kClientW, kClientH };
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRectEx(&rc, style, FALSE, 0);
  HWND hwnd = CreateWindowExW(0, kWndClass, kWndTitle, style,
                              CW_USEDEFAULT, 0, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, g_hInst, nullptr);
  if (!hwnd) return 1;
  ShowWindow(hwnd, showCmd);
  UpdateWindow(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(g_next);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return static_cast<int>(msg.wParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int showCmd) {
  g_hInst = hInst;

  Mode mode = Mode::Gui;
  std::wstring silentDir;
  bool haveDir = false;
  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--uninstall" || a == L"-uninstall" || a == L"/uninstall") mode = Mode::Uninstall;
      else if (a == L"--silent" || a == L"/silent") mode = Mode::Silent;
      else if ((a == L"--dir" || a == L"/dir") && i + 1 < argc) { silentDir = argv[++i]; haveDir = true; }

    }
    LocalFree(argv);
  }
  if (haveDir) g_prefillDir = Trim(silentDir);

  // 单实例互斥
  g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
  const bool already = (GetLastError() == ERROR_ALREADY_EXISTS);
  if (already) {
    if (mode == Mode::Gui) {
      MessageBoxW(nullptr, L"Agent-Heart 安装程序已在运行。", kWndTitle,
                  MB_ICONINFORMATION | MB_OK);
      return 2;
    }
    AttachOrAllocConsole();
    printf("[setup] 另一个安装程序实例正在运行,退出。\n");
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); }
    return 2;
  }

  HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int rc = 0;
  if (mode == Mode::Uninstall) {
    rc = RunUninstall();
  } else if (mode == Mode::Silent) {
    AttachOrAllocConsole();
    std::wstring dir = haveDir && !silentDir.empty() ? Trim(silentDir) : std::wstring(kDefaultDir);
    dir = Trim(ExpandEnv(dir));
    StripTrailingSlash(dir);
    printf("[install] 静默安装到: %ls\n", dir.c_str());
    std::wstring err;
    const DWORD r = RunInstall(dir, true, true, true, false, err);
    if (r == 0) printf("[install] 完成,EstimatedSize=%lu KB\n", g_sizeKB);
    else fwprintf(stderr, L"[install] 失败(%lu): %ls\n", r, err.c_str());
    rc = (r == 0) ? 0 : 1;
  } else {
    rc = RunGui(showCmd);
  }
  if (SUCCEEDED(hrCom)) CoUninitialize();
  if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
  return rc;
}
