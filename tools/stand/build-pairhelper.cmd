@echo off
rem Builds build\pairhelper.exe (.NET Framework 4.x + WinRT metadata from the newest Windows SDK).
setlocal
set FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319
set REF=%ProgramFiles(x86)%\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8
for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\UnionMetadata\10.*" 2^>nul') do if not defined WINMD set "WINMD=%ProgramFiles(x86)%\Windows Kits\10\UnionMetadata\%%d\Windows.winmd"
if not defined WINMD (echo Windows SDK UnionMetadata not found & exit /b 1)
"%FW%\csc.exe" /nologo /nowarn:1701 /target:exe /out:"%~dp0..\..\build\pairhelper.exe" ^
  /r:"%WINMD%" ^
  /r:"%FW%\System.Runtime.WindowsRuntime.dll" ^
  /r:"%REF%\Facades\System.Runtime.dll" ^
  /r:"%REF%\Facades\System.Runtime.InteropServices.WindowsRuntime.dll" ^
  "%~dp0pairhelper.cs"
