# Memory Leak Test Runner for RayV-Paint
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

# Generate a long complex stroke path (30 coordinates) to stress-test the draw cycle
$StrokePath = "10,10;20,20;30,10;40,40;50,10;60,60;70,10;80,80;90,10;100,100;110,10;120,120;130,10;140,140;150,10;160,160;170,10;180,180;190,10;200,200;210,10;220,220;230,10;240,240;250,10;260,260;270,10;280,280;290,10;300,300"
$OutPng = Join-Path $ProjDir "tests\test_mem_output.png"

if (Test-Path $OutPng) {
    Remove-Item $OutPng -Force
}

Write-Host "Running Memory Leak Test (monitoring Working Set)..." -ForegroundColor Cyan

# Start the process asynchronously using .NET Process to get exit code reliably
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ExePath
$psi.Arguments = "--draw-stroke `"$StrokePath`" --width 1024 --height 1024 --export `"$OutPng`""
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$Process = New-Object System.Diagnostics.Process
$Process.StartInfo = $psi
[void]$Process.Start()

# Monitor memory usage while it runs
$MaxMemoryBytes = 0
while (-not $Process.HasExited) {
    try {
        $CurrentMem = $Process.WorkingSet64
        if ($CurrentMem -gt $MaxMemoryBytes) {
            $MaxMemoryBytes = $CurrentMem
        }
    } catch {
        # Process might have just exited
    }
    Start-Sleep -Milliseconds 50
}

# Ensure exit code is fully populated
$Process.WaitForExit()
$ExitCode = $Process.ExitCode
$Process.Close()


# Clean up exported file
if (Test-Path $OutPng) {
    Remove-Item $OutPng -Force
}

$MaxMemoryMB = [Math]::Round($MaxMemoryBytes / 1MB, 2)
Write-Host "Peak Process Memory Usage (Working Set): $MaxMemoryMB MB" -ForegroundColor Yellow

if ($ExitCode -ne 0) {
    Write-Host "Process failed with exit code $ExitCode" -ForegroundColor Red
    exit 1
}


# Assert memory consumption is within limits (e.g. under 350 MB)
$MaxAllowedMB = 350
if ($MaxMemoryMB -gt $MaxAllowedMB) {
    Write-Host "Memory Leak Test Failed: Peak memory usage ($MaxMemoryMB MB) exceeded limit ($MaxAllowedMB MB)." -ForegroundColor Red
    exit 1
} else {
    Write-Host "Memory Leak Test Passed! Memory footprint is stable ($MaxMemoryMB MB)." -ForegroundColor Green
    exit 0
}
