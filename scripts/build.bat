@echo off
rem Developer convenience wrapper: configure and build with MSVC + Ninja.
rem Usage: scripts\build.bat [build-dir] [Debug|Release] [extra cmake args]
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAILED & exit /b 1 )
set BUILD_DIR=%1
if "%BUILD_DIR%"=="" set BUILD_DIR=build
set CONFIG=%2
if "%CONFIG%"=="" set CONFIG=Release
shift
shift
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% %3 %4 %5 %6 %7 %8 %9 || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1
exit /b 0
