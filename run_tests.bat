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

set "EXE_PATH=build\Release\RayVPaint.exe"
if not exist "!EXE_PATH!" (
    echo Error: Executable not found at !EXE_PATH!
    exit /b 1
)

echo Launching executable in test mode (hidden window)...
"!EXE_PATH!" --test
if %ERRORLEVEL% neq 0 (
    echo Test failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo Test completed successfully!
exit /b 0
