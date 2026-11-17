@echo off
rem Agent-Heart installer build (needs VS Build Tools + Windows SDK)
rem Same vswhere + vcvars64 pattern as ..\build.bat; single-file Setup.exe with embedded payload.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [installer] vswhere not found: install VS Build Tools first
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [installer] VS with C++ toolset not found
  exit /b 1
)
rem vcvars64 prints a harmless vswhere message on stderr; keep output quiet.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [installer] vcvars64 failed
  exit /b 1
)
rem Stage the payload for embedding: 101=Agent-Heart.exe 102=LICENSE 103=README.md 104=CHANGELOG.md
if not exist payload mkdir payload
copy /y "..\out\Agent-Heart.exe" "payload\Agent-Heart.exe" >nul 2>&1
copy /y "..\LICENSE" "payload\LICENSE" >nul 2>&1
copy /y "..\README.md" "payload\README.md" >nul 2>&1
copy /y "..\CHANGELOG.md" "payload\CHANGELOG.md" >nul 2>&1
if not exist "payload\Agent-Heart.exe" ( echo [installer] staging failed: ..\out\Agent-Heart.exe & exit /b 1 )
if not exist "payload\LICENSE" ( echo [installer] staging failed: ..\LICENSE & exit /b 1 )
if not exist "payload\README.md" ( echo [installer] staging failed: ..\README.md & exit /b 1 )
if not exist "payload\CHANGELOG.md" ( echo [installer] staging failed: ..\CHANGELOG.md & exit /b 1 )
if not exist out mkdir out
rc /nologo /fo out\setup.res setup.rc
if errorlevel 1 (
  echo [installer] rc compile failed: setup.rc
  exit /b 1
)
rc /nologo /fo out\payload.res payload.rc
if errorlevel 1 (
  echo [installer] rc compile failed: payload.rc
  exit /b 1
)
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX setup.cpp out\setup.res out\payload.res /Foout\ /Fe"Setup.exe" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib
if errorlevel 1 (
  echo [installer] compile failed
  exit /b 1
)
echo [installer] OK: Setup.exe (single file, payload embedded)
exit /b 0
