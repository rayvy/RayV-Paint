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

set "EXE_PATH=build\bin\Release\RayVPaint.exe"
if not exist "!EXE_PATH!" (
    echo Error: Executable not found at !EXE_PATH!
    exit /b 1
)

echo Launching executable in test mode (hidden window)...
"!EXE_PATH!" --test

set "TEST_RESULT=%ERRORLEVEL%"
if %TEST_RESULT% neq 0 (
    echo First test failed with code %TEST_RESULT%
    exit /b %TEST_RESULT%
)

echo Launching executable in headless mode with Python script...
"!EXE_PATH!" --headless --script test_script.py

set "TEST_RESULT=%ERRORLEVEL%"
if %TEST_RESULT% neq 0 (
    echo Python script test failed with code %TEST_RESULT%
    exit /b %TEST_RESULT%
)

echo All tests completed successfully!
exit /b 0
