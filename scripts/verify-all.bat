@echo off
rem verify-all.bat - full self-test suite (no physical input needed)
rem 12 scenarios; each archives out\log-NAME.txt + out\shot-NAME.png,
rem then cleans side effects and restarts the wallpaper. ~90s total.
setlocal
cd /d "%~dp0.."
set "EXE=%~dp0..\out\Agent-Heart.exe"
set "OUT=%~dp0..\out"
if not exist "%EXE%" (
  echo [verify-all] missing %EXE%
  exit /b 1
)
taskkill /IM Agent-Heart.exe /F >nul 2>&1
ping -n 2 127.0.0.1 >nul

call :run scene ""
call :run drawer "--open drawer"
call :run tasks "--open tasks"
call :run hover "--interact hover"
call :run click "--interact click"
call :run cycle "--interact cycle"
call :run n2state "--interact n2state"
call :run tabtest "--interact tabtest"
call :run tilelaunch "--interact tilelaunch"
call :run editmode "--interact editmode"
call :run rowclick "--interact rowclick"
call :run drag "--interact drag"

rem cleanup side effects
taskkill /IM 7zFM.exe /F >nul 2>&1
> "%OUT%\native-wallpaper-state.json" echo {"map":{},"cats":[]}

rem restart wallpaper
start "" /b "%EXE%" --probe
echo [verify-all] done: out\shot-*.png + out\log-*.txt archived; wallpaper restarted
exit /b 0

:run
set "NAME=%~1"
set "EXTRA=%~2"
echo [verify-all] %NAME%
start "" /b "%EXE%" --probe --shot "%OUT%\shot-%NAME%.png" %EXTRA%
ping -n 5 127.0.0.1 >nul
taskkill /IM Agent-Heart.exe /F >nul 2>&1
copy /y "%OUT%\Agent-Heart.log" "%OUT%\log-%NAME%.txt" >nul
ping -n 2 127.0.0.1 >nul
exit /b 0
