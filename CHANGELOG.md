# Changelog

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [1.0.0] - 2026-11-17

首个正式版本。

### Added
- **原生动效壁纸**：768 粒子三层发光球体 + K=12 近邻实时连线网，60fps 满帧渲染，纯 Win32 + Direct2D + Windows.UI.Composition，零 HTML/WebView/第三方运行时
- **球体交互**：拖拽旋转（OrbitControls 阻尼）、悬停汇聚形变 + 相机飞行（0.9s）、点击四环互换 + 焦点虚化（1.0s）、滚轮下滑倒放
- **设置界面**（托盘 → 设置）：动效总开关、球形大小（50%~150% 滑杆）、点击效果开关、鼠标拖尾开关，即时生效并持久化到 %LOCALAPPDATA%\Agent-Heart\config.json
- **桌面集成**：窗口挂载于桌面图标层之下（不遮挡图标/不进任务栏/不抢焦点），托盘菜单（设置/退出），explorer 重启自动重挂，开机自启快捷方式
- **原生数据层**：开始菜单应用目录、运行中任务、五项系统指标全部原生实现（不依赖 HTTP 桥）
- **安装向导 Setup.exe**：选择安装位置、开机自启、桌面/开始菜单快捷方式、进度显示、控制面板卸载项（HKCU Uninstall）
- **便携版**：单文件 Agent-Heart.exe 双击即用
- **工程化**：一键构建 build.bat（图标 + 版本资源）、单文件自检 build-one.bat、12 场景自动回归 scripts/verify-all.bat、图标生成 scripts/make-ico.ps1
- **文档**：README、UI/场景两份移植规格（逐条标注原前端源码行号）

### Notes
- 抽屉与任务面板（原前端功能）代码保留但默认关闭（kOverlaysEnabled=false），当前产品定位为纯动效壁纸
