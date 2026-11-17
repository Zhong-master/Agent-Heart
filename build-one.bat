@echo off
rem build-one.bat source.cpp objdir  - compile only, no link, parallel safe
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~2" mkdir "%~2"
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%WindowsSdkDir%Include\%WindowsTargetPlatformVersion%\cppwinrt" /Isrc /c "%~1" /Fo"%~2\\"
if errorlevel 1 exit /b 1
echo [build-one] OK %~1
exit /b 0
