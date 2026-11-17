@echo off
rem build-release.bat - full release build: app + installer + dist packaging
setlocal
cd /d "%~dp0"
call build.bat
if errorlevel 1 exit /b 1
call installer\build-installer.bat
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-dist.ps1
if errorlevel 1 exit /b 1
echo [release] OK - see dist\
exit /b 0
