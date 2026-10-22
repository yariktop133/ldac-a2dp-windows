@echo off
rem Builds build\pairhelper.exe (.NET Framework 4.x + WinRT metadata from the Windows SDK).
set FW=C:\Windows\Microsoft.NET\Framework64\v4.0.30319
set REF=C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8
"%FW%\csc.exe" /nologo /nowarn:1701 /target:exe /out:"%~dp0..\..\build\pairhelper.exe" ^
  /r:"C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.28000.0\Windows.winmd" ^
  /r:"%FW%\System.Runtime.WindowsRuntime.dll" ^
  /r:"%REF%\Facades\System.Runtime.dll" ^
  /r:"%REF%\Facades\System.Runtime.InteropServices.WindowsRuntime.dll" ^
  "%~dp0pairhelper.cs"
