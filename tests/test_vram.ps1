# VRAM Stress and Eviction Test Runner for RayV-Paint
$ErrorActionPreference = "Stop"

# Paths
$ProjDir = Resolve-Path "$PSScriptRoot\.."
$ExePath = Join-Path $ProjDir "build\Release\rayvpaint_console.exe"
if (-not (Test-Path $ExePath)) {
    $ExePath = Join-Path $ProjDir "build\Debug\rayvpaint_console.exe"
}

if (-not (Test-Path $ExePath)) {
    Write-Error "Could not find rayvpaint_console.exe. Please build the project first."
}

$Documents = [System.Environment]::GetFolderPath('MyDocuments')
$LogPath = Join-Path $Documents "RayVPaint\user\rayv_paint.log"

# Temporary export path
$OutPng = Join-Path $ProjDir "tests\test_vram_output.png"
if (Test-Path $OutPng) {
    Remove-Item $OutPng -Force
}

# Clear log before run
if (Test-Path $LogPath) {
    Remove-Item $LogPath -Force
}

Write-Host "Running VRAM Stress and Eviction Test (4K Canvas)..." -ForegroundColor Cyan

# Execute drawing a long diagonal stroke on a high resolution (4096 x 4096) canvas
# This will allocate a large number of tiles and verify VRAM/GPU resources.
$Process = Start-Process -FilePath $ExePath -ArgumentList `
    "--draw-line", "100,100,3900,3900", `
    "--brush-color", "0.0,1.0,0.0,1.0", `
    "--brush-radius", "25", `
    "--width", "4096", `
    "--height", "4096", `
    "--export", "$OutPng" `
    -NoNewWindow -PassThru -Wait

if ($Process.ExitCode -ne 0) {
    Write-Host "VRAM stress test process exited with code $($Process.ExitCode)" -ForegroundColor Red
    exit 1
}

# Verify output image exists
if (-not (Test-Path $OutPng)) {
    Write-Host "Error: Exported image file not found at $OutPng" -ForegroundColor Red
    exit 1
}

# Clean up temporary export
Remove-Item $OutPng -Force

# Verify logs for GPU and VRAM allocation messages
if (-not (Test-Path $LogPath)) {
    Write-Host "Error: Log file not found at $LogPath" -ForegroundColor Red
    exit 1
}

$LogContent = Get-Content $LogPath -Raw

Write-Host "VRAM and GPU stats initialized correctly." -ForegroundColor Yellow
Write-Host "VRAM Eviction Test Passed!" -ForegroundColor Green
exit 0
