@echo off
rem Sets up the x64 MSVC environment of the newest Visual Studio with C++ tools
rem (found through vswhere), unless a developer prompt is already active.
if defined VCToolsInstallDir exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio with "Desktop development with C++" & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (echo No Visual Studio with C++ tools found & exit /b 1)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
