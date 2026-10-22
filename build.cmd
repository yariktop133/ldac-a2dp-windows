@echo off
rem Configure and build the user-mode parts (CMake + Ninja from Visual Studio) and run the tests.
call "%~dp0tools\vsenv.cmd" || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build "%~dp0build" || exit /b 1
ctest --test-dir "%~dp0build" --output-on-failure
