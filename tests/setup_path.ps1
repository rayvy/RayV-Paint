# Setup User PATH Environment Variable for RayV-Paint
$ErrorActionPreference = "Stop"

$ProjDir = Resolve-Path "$PSScriptRoot"
$ReleaseDir = Join-Path $ProjDir "build\Release"

Write-Host "Setting up RayV-Paint Environment PATH..." -ForegroundColor Cyan
Write-Host "Target directory: $ReleaseDir" -ForegroundColor Yellow

# Verify targets exist
if (-not (Test-Path (Join-Path $ReleaseDir "rayvpaint.exe"))) {
    Write-Host "Warning: rayvpaint.exe not found in $ReleaseDir. Make sure to build before running setup." -ForegroundColor Yellow
}

# Fetch current User PATH
$UserPath = [System.Environment]::GetEnvironmentVariable("Path", "User")
$Paths = $UserPath -split ";" | Where-Object { $_ -ne "" }

# Standardize path comparison (trailing slashes, case)
$TargetFolderNormalized = [System.IO.Path]::GetFullPath($ReleaseDir).TrimEnd('\').ToLower()
$AlreadyExists = $false

foreach ($P in $Paths) {
    if ($P -ne "") {
        $PNormalized = [System.IO.Path]::GetFullPath($P).TrimEnd('\').ToLower()
        if ($PNormalized -eq $TargetFolderNormalized) {
            $AlreadyExists = $true
            break
        }
    }
}

if ($AlreadyExists) {
    Write-Host "Path '$ReleaseDir' is already in your environment PATH." -ForegroundColor Green
} else {
    $NewPath = ($Paths + $ReleaseDir) -join ";"
    [System.Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "Success! Added '$ReleaseDir' to User Environment Path." -ForegroundColor Green
    Write-Host "Please restart your terminal/cmd/powershell to apply PATH changes." -ForegroundColor Yellow
}
