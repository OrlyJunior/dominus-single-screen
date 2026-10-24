@echo off
setlocal
rem Dominus Single Screen uninstaller. Close the game first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Uninstall-DominusSingleScreen.ps1" %*
echo.
pause
