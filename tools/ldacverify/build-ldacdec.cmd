@echo off
rem Builds build\ldacdec_wav.exe (open-source LDAC decoder, for verification only).
call "%~dp0..\vsenv.cmd" || exit /b 1
python -I "%~dp0build-ldacdec.py"
