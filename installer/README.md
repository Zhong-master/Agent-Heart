# Agent-Heart 安装向导(Setup.exe)

纯原生 Win32 手写安装向导,不使用 Inno Setup / NSIS / WiX 等任何第三方安装器框架。
全部由 `setup.cpp`(C++20、UNICODE)+ `setup.rc`(图标/版本信息)+ `build-installer.bat` 构建。

## 构建

在 `installer\` 目录下执行:

```bat
build-installer.bat
```

脚本与根目录 `build.bat` 同一套写法:vswhere 定位 VS Build Tools → `vcvars64.bat` → `rc` 编译
`setup.rc`(引入 `..\assets\wallpaper.ico`)→ `cl` 编译 `setup.cpp` 并链接
`user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib`,
输出 **`installer\Setup.exe`**。

## 界面主题(深色现代风,纯 GDI 自绘)

- 配色:窗口底 `#0E1116`、卡片 `#161A21`、主文字 `#E8EAED`、次级文字 ≈ white 55%、描边 ≈ white 14%,
  强调色渐变 `#5B7CFA → #8A5CF6`(悬停 `#6E8BFF`、按下更深);由 `WM_CTLCOLORSTATIC/WM_CTLCOLORBTN/WM_CTLCOLOREDIT` 统一。
- 头部:28px 程序图标 + 22pt 粗标题 + 副标题 + 3px 强调渐变横条;欢迎页左侧 48px 大图标(DrawIconEx,资源 id=1)。
- 按钮/复选框均为 `BS_OWNERDRAW` 自绘:圆角 8px、常态/悬停/按下/禁用四态,主按钮(下一步/安装/完成)渐变填充白字,
  复选框强调色对勾 + 悬停反馈(`WM_MOUSEMOVE`/`WM_MOUSELEAVE` 子类跟踪);进度条为父窗口自绘圆角渐变。
- 字体 Segoe UI:标题 22 / 区块 16 / 正文 13 / 按钮 13;Tab/回车(默认按钮)/ESC 行为保留。

## 向导步骤(中文界面,窗口使用 wallpaper.ico 图标)

1. **欢迎页**:产品名 Agent-Heart、版本 1.0.0.0、说明文字。
2. **安装位置页**:编辑框默认 `%LOCALAPPDATA%\Programs\Agent-Heart`(使用时展开环境变量),
   带“浏览”按钮(`SHBrowseForFolder`);拒绝空路径与相对路径。
3. **选项页**(三个复选框,默认全选):
   - 开机自启 → 在 `shell:startup` 创建 `Agent-Heart.lnk`,指向安装目录的 `Agent-Heart.exe`;
   - 创建桌面快捷方式 → 桌面 `Agent-Heart.lnk`;
   - 创建开始菜单快捷方式 → 开始菜单“程序”下的 `Agent-Heart\Agent-Heart.lnk`(程序文件夹)。
4. **安装页**:遍历复制 **Setup.exe 同级的 `payload` 目录**里的全部文件到安装目录,
   进度条按**文件计数**显示“已复制 n/total + 文件名”。进入该页即开始安装,期间“上一步/取消/安装”均禁用。
5. **完成页**:可选“立即运行 Agent-Heart.exe”(默认勾选),点“完成”退出。

> payload 目录由后续放置,安装器只负责遍历复制;必须包含 `Agent-Heart.exe`(否则给出明确错误)。

## 注册表卸载项

安装成功后写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agent-Heart`:

| 值 | 内容 |
|---|---|
| DisplayName | Agent-Heart |
| DisplayVersion | 1.0.0.0 |
| DisplayIcon | `<安装目录>\Agent-Heart.exe` |
| Publisher | Agent-Heart |
| InstallLocation | 安装目录 |
| UninstallString / QuietUninstallString | `"<Setup.exe 完整路径>" --uninstall` |
| EstimatedSize | payload 文件总大小(KB,REG_DWORD) |
| NoModify / NoRepair | 1 |

## 命令行参数

- `Setup.exe --uninstall` —— **静默卸载**:读取注册表 `InstallLocation`,删除已知文件
  (`Agent-Heart.exe`、`LICENSE`、`README.md`)、三个快捷方式(桌面 / 启动文件夹 / 开始菜单程序文件夹)、
  非空则保留安装目录并给出提示,最后删除 HKCU 卸载注册表项;尽量附着父控制台输出日志,返回 0/1。
- `Setup.exe --silent [--dir <目录>]` —— 静默安装(自测用,三个快捷方式全建);不给 `--dir` 时用默认目录。
- `--dir <目录>` 单独使用时,图形向导会**预填安装位置编辑框**(便于测试)。
- 其它参数或无参数 → 图形向导。

## 行为与安全约束

- **单实例**:`Local\AgentHeartSetupSingleInstance` 互斥,重复启动会提示并退出。
- 错误(找不到 payload、复制失败、快捷方式失败、注册表失败)都用中文对话框/控制台明确提示,
  安装页可“重试”,安装中禁止关闭窗口。
- 只写:安装目标目录、`HKCU` Uninstall 键、`shell:startup` 快捷方式、桌面与开始菜单快捷方式。
- **不碰**:HKLM、Winlogon、系统目录、服务、计划任务、环境变量;全程无需管理员权限。

## 自测回路(已在本机跑通)

```bat
Setup.exe --silent --dir "%TEMP%\AgentHeartSetupTest"   :: 安装
:: 检查:3 个文件 / 桌面+启动+开始菜单 3 个 .lnk / HKCU Uninstall 键各值
Setup.exe --uninstall                                     :: 卸载
:: 检查:目录、快捷方式、注册表键全部消失
```

图形向导实测:逐页 CopyFromScreen 截图目检(欢迎/安装位置/选项/完成)→ 下一步到选项页 → 上一步返回 →
点掉“创建桌面快捷方式”复选框 → 安装(桌面 lnk 未建、自启/开始菜单 lnk 已建,证明选项生效)→ 完成页点“完成”,
勾选“立即运行”会启动安装目录里的程序副本(并被单实例互斥拦下)→ 向导进程正常退出 → `--uninstall` 清理,
目录/三个快捷方式/HKCU 卸载键全部消失,测试残留已删净。
