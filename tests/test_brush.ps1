# Brush Emulation Test Runner for RayV-Paint
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

# Output image path
$OutPng = Join-Path $ProjDir "tests\test_line.png"
if (Test-Path $OutPng) {
    Remove-Item $OutPng -Force
}

Write-Host "Running Brush Emulation Test..." -ForegroundColor Cyan

# Execute drawing line from (10,10) to (200,200) with red color
$Process = Start-Process -FilePath $ExePath -ArgumentList `
    "--draw-line", "10,10,200,200", `
    "--brush-color", "1.0,0.0,0.0,1.0", `
    "--brush-radius", "8", `
    "--brush-hardness", "0.9", `
    "--width", "256", `
    "--height", "256", `
    "--export", "$OutPng" `
    -NoNewWindow -PassThru -Wait

if ($Process.ExitCode -ne 0) {
    Write-Host "Brush emulation process exited with code $($Process.ExitCode)" -ForegroundColor Red
    exit 1
}

# Verify output image exists
if (-not (Test-Path $OutPng)) {
    Write-Host "Error: Exported image file not found at $OutPng" -ForegroundColor Red
    exit 1
}

# Read pixel at (100,100) to verify it is red
Add-Type -AssemblyName System.Drawing
$Bmp = New-Object System.Drawing.Bitmap($OutPng)
$Pixel = $Bmp.GetPixel(100, 100)
$Bmp.Dispose()

Write-Host "Sampled Pixel at (100,100): R=$($Pixel.R), G=$($Pixel.G), B=$($Pixel.B), A=$($Pixel.A)" -ForegroundColor Yellow

# Verify Red channel is high, and Green/Blue are low (since we drew with 1.0, 0.0, 0.0)
if ($Pixel.R -gt 200 -and $Pixel.G -lt 50 -and $Pixel.B -lt 50 -and $Pixel.A -gt 200) {
    Write-Host "Brush Emulation Test Passed: Correct red stroke detected at (100,100)!" -ForegroundColor Green
    # Clean up output
    Remove-Item $OutPng -Force
    exit 0
} else {
    Write-Host "Brush Emulation Test Failed: Drawn pixel at (100,100) does not match expected red color." -ForegroundColor Red
    exit 1
}
