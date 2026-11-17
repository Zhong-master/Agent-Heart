@echo off
rem Agent-Heart one-click build (needs VS Build Tools + Windows SDK)
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [build] vswhere not found: install VS Build Tools first
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [build] VS with C++ toolset not found
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [build] vcvars64 failed
  exit /b 1
)
if not exist out mkdir out
rc /nologo /fo out\wallpaper.res src\wallpaper.rc
if errorlevel 1 (
  echo [build] rc compile failed
  exit /b 1
)
set SRCS=src\main.cpp src\sphere_scene.cpp src\app_data.cpp src\ui_overlay.cpp out\wallpaper.res
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%WindowsSdkDir%Include\%WindowsTargetPlatformVersion%\cppwinrt" %SRCS% /Foout\ /Fe"out\Agent-Heart.exe" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib windowsapp.lib d2d1.lib d3d11.lib comctl32.lib ole32.lib oleaut32.lib advapi32.lib pdh.lib iphlpapi.lib psapi.lib CoreMessaging.lib dwrite.lib oleacc.lib shlwapi.lib windowscodecs.lib uuid.lib
if errorlevel 1 (
  echo [build] compile failed
  exit /b 1
)
echo [build] OK: out\Agent-Heart.exe
exit /b 0
