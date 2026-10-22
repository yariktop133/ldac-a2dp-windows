@echo off
rem Builds build\ldacdec_wav.exe (open-source LDAC decoder, for verification only).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
python -I "%~dp0build-ldacdec.py"
