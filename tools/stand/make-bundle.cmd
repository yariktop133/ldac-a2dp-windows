@echo off
rem Collects everything the test VM needs into build\stand\ (copy that folder into the VM).
rem Usage: tools\stand\make-bundle.cmd [Debug|Release]
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Debug
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\stand
set PKG=%ROOT%\build\LdacA2dp\%CFG%\LdacA2dp

if not exist "%PKG%\LdacA2dp.sys" (echo Build the driver first: build_driver.cmd %CFG% & exit /b 1)
if not exist "%ROOT%\build\ldacsvc.exe" (echo Build the service first: build.cmd & exit /b 1)

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1
copy /y "%PKG%\LdacA2dp.sys" "%OUT%" >nul
copy /y "%PKG%\LdacA2dp.inf" "%OUT%" >nul
copy /y "%PKG%\ldaca2dp.cat" "%OUT%" >nul
if exist "%PKG%\LdacA2dpTest.cer" (copy /y "%PKG%\LdacA2dpTest.cer" "%OUT%" >nul) else (echo warning: package not signed yet, run tools\stand\sign-driver.ps1)
copy /y "%ROOT%\build\LdacA2dp\%CFG%\LdacA2dp.pdb" "%OUT%" >nul
copy /y "%ROOT%\build\ldacsvc.exe" "%OUT%" >nul
copy /y "%~dp0install-on-vm.ps1" "%OUT%" >nul
copy /y "%ROOT%\docs\test-stand.md" "%OUT%" >nul
echo Bundle ready: %OUT%
dir /b "%OUT%"
