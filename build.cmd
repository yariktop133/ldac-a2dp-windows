@echo off
rem Configure and build with the CMake/Ninja bundled in Visual Studio.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build "%~dp0build" || exit /b 1
ctest --test-dir "%~dp0build" --output-on-failure
