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

set "SUITE=%~1"
if "%SUITE%"=="" set "SUITE=smoke"

if /I "%SUITE%"=="smoke" goto :smoke
if /I "%SUITE%"=="unusual" goto :unusual
if /I "%SUITE%"=="16k" goto :16k
if /I "%SUITE%"=="all" goto :all

echo Unknown suite "%SUITE%". Use: smoke ^| unusual ^| 16k ^| all
exit /b 1

:smoke
echo --- Suite: smoke ---
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

echo Smoke tests completed successfully!
exit /b 0

:unusual
echo --- Suite: unusual (edge cases / exotic formats) ---
"!EXE_PATH!" --headless --script test_unusual_scenarios.py
set "TEST_RESULT=%ERRORLEVEL%"
if %TEST_RESULT% neq 0 (
    echo Unusual suite failed with code %TEST_RESULT%
    exit /b %TEST_RESULT%
)
echo Unusual suite completed successfully!
exit /b 0

:16k
echo --- Suite: 16k (heavy) ---
"!EXE_PATH!" --test-16k
set "TEST_RESULT=%ERRORLEVEL%"
if %TEST_RESULT% neq 0 (
    echo 16K test failed with code %TEST_RESULT%
    exit /b %TEST_RESULT%
)
echo 16K suite completed successfully!
exit /b 0

:all
call "%~f0" smoke
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
call "%~f0" unusual
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
call "%~f0" 16k
exit /b %ERRORLEVEL%
