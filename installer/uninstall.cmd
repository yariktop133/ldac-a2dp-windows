@echo off
rem LDAC A2DP: removal (asks for administrator rights). Settings are kept;
rem "uninstall.ps1 -RemoveSettings" deletes them too.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
echo.
pause
