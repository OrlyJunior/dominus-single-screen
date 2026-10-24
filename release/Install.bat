@echo off
setlocal
rem Dominus Single Screen installer. Close the game first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Install-DominusSingleScreen.ps1" %*
echo.
pause
