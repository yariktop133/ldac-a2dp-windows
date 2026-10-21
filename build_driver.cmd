@echo off
rem Build the KMDF driver (WDK 10.0.28000 + Visual Studio 2026).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
msbuild "%~dp0driver\LdacA2dp\LdacA2dp.vcxproj" -nologo -v:minimal -p:Configuration=%1 -p:Platform=x64 %2 %3 %4
