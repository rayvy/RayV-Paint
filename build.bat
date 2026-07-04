@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo Building RayVPaint...
echo ===================================================

:: Check for Visual Studio 2022 vcvarsall.bat
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "!VCVARS!" (
    echo Error: Visual Studio 2022 Community not found at the default location:
    echo "!VCVARS!"
    echo Please check your Visual Studio installation.
    exit /b 1
)

:: Find CMake executable bundled with VS 2022
set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist "!CMAKE_EXE!" (
    echo Error: Bundled CMake not found at:
    echo "!CMAKE_EXE!"
    exit /b 1
)

echo Setting up Visual Studio compilation environment (x64)...
call "!VCVARS!" x64

echo Generating CMake build files...
"!CMAKE_EXE!" -B build -S . -DCMAKE_BUILD_TYPE=Release

echo Compiling project...
"!CMAKE_EXE!" --build build --config Release

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b %ERRORLEVEL%
)

echo Build succeeded! Executive can be found at build\bin\Release\RayVPaint.exe
exit /b 0
