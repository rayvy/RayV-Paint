# Unit and Stability Test Runner for RayV-Paint
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

Write-Host "Running Unit Tests with $ExePath..." -ForegroundColor Cyan

# Remove old log if exists
if (Test-Path $LogPath) {
    Remove-Item $LogPath -Force
}

# Run the engine test mode
$Process = Start-Process -FilePath $ExePath -ArgumentList "--test", "--headless" -NoNewWindow -PassThru -Wait

if ($Process.ExitCode -ne 0) {
    Write-Host "Unit test process exited with code $($Process.ExitCode)" -ForegroundColor Red
    exit 1
}

# Verify logs
if (-not (Test-Path $LogPath)) {
    Write-Host "Error: Log file not found at $LogPath" -ForegroundColor Red
    exit 1
}

$LogContent = Get-Content $LogPath -Raw

$RequiredPhrases = @(
    "Bucket Fill test succeeded",
    "Magic Wand test succeeded",
    "Magic Wand non-contiguous completed successfully",
    "Magic Wand cancellation succeeded"
)

$Passed = $true
foreach ($Phrase in $RequiredPhrases) {
    if ($LogContent -notmatch [regex]::Escape($Phrase)) {
        Write-Host "Missing log message: '$Phrase'" -ForegroundColor Red
        $Passed = $false
    }
}

if ($Passed) {
    Write-Host "Unit tests completed successfully!" -ForegroundColor Green
    exit 0
} else {
    Write-Host "Unit tests failed visual/logical assertions in logs." -ForegroundColor Red
    exit 1
}
