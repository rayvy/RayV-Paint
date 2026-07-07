@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo Running RayVPaint Autotests...
echo ===================================================

:: Ensure build is up-to-date
call build.bat
if %ERRORLEVEL% neq 0 (
    echo Error: Build failed, cannot run tests.
    exit /b %ERRORLEVEL%
)

set "EXE_PATH=build\Release\rayvpaint_console.exe"
if not exist "!EXE_PATH!" (
    echo Error: Executable not found at !EXE_PATH!
    exit /b 1
)

echo Launching automated PowerShell test suite...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tests\run_tests.ps1"
exit /b %ERRORLEVEL%

