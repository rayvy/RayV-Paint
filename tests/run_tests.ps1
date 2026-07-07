# Master Automated Test Suite Runner for RayV-Paint
$ErrorActionPreference = "Continue"

$ProjDir = Resolve-Path "$PSScriptRoot\.."
$TestsDir = Join-Path $ProjDir "tests"

$Tests = @(
    @{ Name = "Unit / Logic Tests"; Script = "test_unit.ps1" },
    @{ Name = "Brush Emulation";    Script = "test_brush.ps1" },
    @{ Name = "VRAM / Eviction";    Script = "test_vram.ps1" },
    @{ Name = "Memory Leak / RAM";  Script = "test_memory_leak.ps1" }
)

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Starting RayV-Paint Automated Test Suite" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan

$Results = @()
$AnyFailed = $false

foreach ($Test in $Tests) {
    $ScriptPath = Join-Path $TestsDir $Test.Script
    Write-Host "`n[RUN] Running $($Test.Name)..." -ForegroundColor Yellow
    
    $Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    
    # Run script
    $Process = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$ScriptPath" -NoNewWindow -PassThru -Wait
    
    $Stopwatch.Stop()
    $Duration = [Math]::Round($Stopwatch.Elapsed.TotalSeconds, 2)
    
    $Status = "PASS"
    $Color = "Green"
    if ($Process.ExitCode -ne 0) {
        $Status = "FAIL"
        $Color = "Red"
        $AnyFailed = $true
    }
    
    Write-Host "[$Status] $($Test.Name) finished in $Duration seconds" -ForegroundColor $Color
    
    $Results += [PSCustomObject]@{
        "Test Name" = $Test.Name
        "Status"    = $Status
        "Time (s)"  = $Duration
    }
}

Write-Host "`n===================================================" -ForegroundColor Cyan
Write-Host "Test Execution Summary" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan

$Results | Format-Table -AutoSize

if ($AnyFailed) {
    Write-Host "TEST SUITE FAILED! Some tests did not pass." -ForegroundColor Red
    exit 1
} else {
    Write-Host "ALL TESTS PASSED SUCCESSFULLY!" -ForegroundColor Green
    exit 0
}
