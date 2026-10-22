@echo off
rem Build the KMDF driver: build_driver.cmd Debug|Release. Needs the WDK (10.0.26100 or later,
rem with ACX 1.1) and the "Windows Driver Kit" Visual Studio component.
call "%~dp0tools\vsenv.cmd" || exit /b 1
msbuild "%~dp0driver\LdacA2dp\LdacA2dp.vcxproj" -nologo -v:minimal -p:Configuration=%1 -p:Platform=x64 %2 %3 %4
