@echo off
rem verify-shots.bat - five-scene screenshot matrix (wallpaper mode)
rem Each run: start exe, wait, capture surface shot at frame 90, kill.
setlocal
cd /d "%~dp0.."
set "EXE=%~dp0..\out\Agent-Heart.exe"
set "OUT=%~dp0..\out"
if not exist "%EXE%" (
  echo [verify] exe missing: %EXE%
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

call :run scene ""
call :run drawer "--open drawer"
call :run tasks "--open tasks"
call :run hover "--interact hover"
call :run click "--interact click"
echo [verify] done: out\shot-*.png
exit /b 0

:run
set "NAME=%~1"
set "EXTRA=%~2"
echo [verify] run %NAME%
start "" /b "%EXE%" --probe --shot "%OUT%\shot-%NAME%.png" %EXTRA%
ping -n 4 127.0.0.1 >nul
taskkill /IM Agent-Heart.exe /F >nul 2>&1
ping -n 2 127.0.0.1 >nul
exit /b 0
