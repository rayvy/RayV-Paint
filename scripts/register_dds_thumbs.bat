@echo off
REM Register RayVPaint DDS thumbnail handler for Explorer (HKLM — requires Admin once).
REM Also restores PNG/JPG thumbs if Google Drive hijacked them (HKCU).
setlocal EnableExtensions
set "DLL=%~dp0..\build\Release\bin\RayVPaint_DdsThumb.dll"
if not exist "%DLL%" set "DLL=%~dp0..\build\Release\RayVPaint_DdsThumb.dll"
if not exist "%DLL%" (
  echo [ERR] RayVPaint_DdsThumb.dll not found. Build target RayVPaint_DdsThumb first.
  exit /b 1
)

echo DLL: %DLL%
echo.
echo Requesting Administrator for HKLM registration (required by Explorer thumbnail host)...
powershell -NoProfile -Command "Start-Process regsvr32.exe -ArgumentList '/s','\"%DLL%\"' -Verb RunAs -Wait"
if errorlevel 1 (
  echo [WARN] Elevated regsvr32 failed or was cancelled.
)

REM Non-elevated pass: extension bindings + PNG restore via DLL export
powershell -NoProfile -Command ^
  "$d='%DLL%'; $m=[Reflection.Assembly]::LoadFrom does not work for native; " ^
  "Add-Type -TypeDefinition ('using System;using System.Runtime.InteropServices;public class R{[DllImport(@\"'+$d.Replace('\','\\')+'\",EntryPoint=\"RayV_RegisterDdsThumbnails\",CallingConvention=CallingConvention.StdCall)]public static extern int Reg();}'); [R]::Reg() | Out-Null"

echo.
echo Registry check:
reg query "HKLM\SOFTWARE\Classes\CLSID\{B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}\InprocServer32" 2>nul
reg query "HKCU\Software\Classes\.dds\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}" 2>nul
reg query "HKCU\Software\Classes\.png\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}" 2>nul

echo.
echo If previews still wrong: restart Explorer, clear thumbcache_*.db, use Large icons view.
endlocal
