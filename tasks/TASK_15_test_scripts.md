# TASK 15 — Тестовые скрипты (Benchmark + Crash Detection)

**Файлы:** `src/launchers/` [NEW scripts], `CMakeLists.txt`
**Приоритет:** 🟡 СРЕДНИЙ (необходим для верификации задач)
**Сложность:** Низкая

---

## Цель

AI агенты и разработчик должны иметь возможность запустить скрипты для:
1. Проверки что крашей нет при resize / открытии больших файлов
2. Замера FPS и latency для разных размеров canvas
3. Проверки VRAM usage

Приложение уже поддерживает `--test` и `--headless` флаги с тестовым режимом.

---

## Скрипт 1: test_stability.ps1

```powershell
# test_stability.ps1 — Базовый тест стабильности
param(
    [string]$ExePath = ".\build\Release\bin\RayVPaint.exe",
    [string]$TestImage = ""  # необязательно
)

Write-Host "=== RayVPaint Stability Test ===" -ForegroundColor Cyan

# Тест 1: базовый запуск + выход
Write-Host "[1/3] Basic headless launch..." -ForegroundColor Yellow
$proc = Start-Process -FilePath $ExePath -ArgumentList "--headless", "--console" -PassThru -Wait -NoNewWindow
if ($proc.ExitCode -eq 0) {
    Write-Host "  PASS: Clean exit" -ForegroundColor Green
} else {
    Write-Host "  FAIL: Exit code $($proc.ExitCode)" -ForegroundColor Red
    exit 1
}

# Тест 2: запуск с тестовым изображением (если задан)
if ($TestImage -ne "" -and (Test-Path $TestImage)) {
    Write-Host "[2/3] Launch with image: $TestImage" -ForegroundColor Yellow
    $proc = Start-Process -FilePath $ExePath -ArgumentList "--headless", "--console", $TestImage -PassThru -Wait -NoNewWindow
    if ($proc.ExitCode -eq 0) {
        Write-Host "  PASS: Image loaded and exited cleanly" -ForegroundColor Green
    } else {
        Write-Host "  FAIL: Crashed with code $($proc.ExitCode)" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[2/3] SKIP: No test image specified" -ForegroundColor Gray
}

# Тест 3: проверить лог на наличие ошибок
Write-Host "[3/3] Checking log for errors..." -ForegroundColor Yellow
$logPath = "$env:USERPROFILE\AppData\Roaming\RayVPaint\user\rayv_paint.log"
if (Test-Path $logPath) {
    $errors = Select-String -Path $logPath -Pattern "\[ERROR\]" -SimpleMatch
    if ($errors.Count -eq 0) {
        Write-Host "  PASS: No errors in log" -ForegroundColor Green
    } else {
        Write-Host "  WARN: $($errors.Count) error(s) found in log:" -ForegroundColor Yellow
        $errors | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Yellow }
    }
} else {
    Write-Host "  SKIP: Log file not found" -ForegroundColor Gray
}

Write-Host ""
Write-Host "=== Stability Test Complete ===" -ForegroundColor Cyan
```

## Скрипт 2: benchmark_canvas.ps1

```powershell
# benchmark_canvas.ps1 — Замер производительности
param(
    [string]$ExePath = ".\build\Release\bin\RayVPaint.exe",
    [string]$ResultsFile = "benchmark_results.txt"
)

Write-Host "=== RayVPaint Canvas Benchmark ===" -ForegroundColor Cyan

$sizes = @(512, 1024, 2048, 4096)
$results = @()

foreach ($size in $sizes) {
    Write-Host "Testing ${size}x${size}..." -ForegroundColor Yellow
    
    # Запустить в тестовом режиме — приложение само создаёт canvas такого размера
    $startTime = Get-Date
    $proc = Start-Process -FilePath $ExePath `
        -ArgumentList "--test", "--headless", "--console" `
        -PassThru -Wait -NoNewWindow `
        -RedirectStandardOutput "bench_${size}_stdout.txt" `
        -RedirectStandardError "bench_${size}_stderr.txt"
    $elapsed = (Get-Date) - $startTime
    
    $result = [PSCustomObject]@{
        Size      = "${size}x${size}"
        ExitCode  = $proc.ExitCode
        WallTimeMs = [math]::Round($elapsed.TotalMilliseconds)
        Status    = if ($proc.ExitCode -eq 0) { "PASS" } else { "FAIL" }
    }
    $results += $result
    
    # Парсить лог для startup time
    $logPath = "$env:USERPROFILE\AppData\Roaming\RayVPaint\user\rayv_paint.log"
    if (Test-Path $logPath) {
        $startupLine = Select-String -Path $logPath -Pattern "Startup completed in:" | Select-Object -Last 1
        if ($startupLine) {
            Write-Host "  Startup: $($startupLine.Line)" -ForegroundColor Gray
        }
    }
    
    Write-Host "  $($result.Status) — Wall time: $($result.WallTimeMs)ms" -ForegroundColor $(if ($result.Status -eq "PASS") { "Green" } else { "Red" })
}

# Записать результаты
$results | Format-Table | Out-String | Set-Content $ResultsFile
Write-Host ""
Write-Host "Results saved to: $ResultsFile" -ForegroundColor Cyan
$results | Format-Table
```

## Скрипт 3: check_vram.ps1

```powershell
# check_vram.ps1 — Проверка VRAM usage через GPU info
param(
    [string]$ExePath = ".\build\Release\bin\RayVPaint.exe"
)

Write-Host "=== VRAM Check ===" -ForegroundColor Cyan

# Получить текущий GPU
$gpuInfo = Get-WmiObject Win32_VideoController | Select-Object Name, AdapterRAM
foreach ($gpu in $gpuInfo) {
    $vramGB = [math]::Round($gpu.AdapterRAM / 1GB, 2)
    Write-Host "GPU: $($gpu.Name) — VRAM: ${vramGB}GB" -ForegroundColor White
}

Write-Host ""
Write-Host "Launch RayVPaint and check status bar for real-time VRAM usage." -ForegroundColor Yellow
Write-Host "Expected after TASK_14: VRAM info visible in bottom status bar." -ForegroundColor Yellow

# Проверить лог на VRAM budget строки
$logPath = "$env:USERPROFILE\AppData\Roaming\RayVPaint\user\rayv_paint.log"
if (Test-Path $logPath) {
    $vramLines = Select-String -Path $logPath -Pattern "VRAM Budget:" -SimpleMatch
    if ($vramLines) {
        Write-Host ""
        Write-Host "From log (VRAM Budget):" -ForegroundColor Cyan
        $vramLines | Select-Object -Last 3 | ForEach-Object { Write-Host "  $($_.Line)" }
    }
}
```

## Скрипт 4: run_all_tests.ps1

```powershell
# run_all_tests.ps1 — Запустить все тесты
param(
    [string]$ExePath = ".\build\Release\bin\RayVPaint.exe",
    [string]$TestImage4K = ""
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  RayVPaint Full Test Suite" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$failed = 0

# Stability
Write-Host "--- STABILITY ---" -ForegroundColor Magenta
& ".\src\launchers\test_stability.ps1" -ExePath $ExePath -TestImage $TestImage4K
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Host ""

# Benchmark
Write-Host "--- BENCHMARK ---" -ForegroundColor Magenta
& ".\src\launchers\benchmark_canvas.ps1" -ExePath $ExePath
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Host ""
Write-Host "========================================"
if ($failed -eq 0) {
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "$failed TEST(S) FAILED" -ForegroundColor Red
    exit 1
}
```

---

## CMakeLists.txt — добавить custom target

```cmake
# Добавить в CMakeLists.txt:
add_custom_target(run_tests
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/src/launchers/run_all_tests.ps1"
        -ExePath "$<TARGET_FILE:RayVPaint>"
    DEPENDS RayVPaint
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running RayVPaint test suite"
)
```

Тогда можно запускать через:
```
cmake --build build --target run_tests
```

---

## Расположение файлов

```
src/launchers/
├── test_stability.ps1
├── benchmark_canvas.ps1
├── check_vram.ps1
└── run_all_tests.ps1
```

---

## Как AI агент запускает тесты

После каждой задачи (TASK_11, 12, 13...) агент должен:

1. Собрать проект:
```powershell
cmake --build build --config Release
```

2. Запустить базовый тест:
```powershell
.\src\launchers\test_stability.ps1 -ExePath ".\build\Release\bin\RayVPaint.exe"
```

3. Для задач связанных с 4K — если есть тестовое изображение:
```powershell
.\src\launchers\test_stability.ps1 -ExePath ".\build\Release\bin\RayVPaint.exe" -TestImage "path\to\test_4096x4096.png"
```

4. Проверить exit code = 0 и отсутствие [ERROR] в логе.

---

## INPUT для агента

**Файлы для чтения:**
- `src/launchers/` — проверить что директория существует (или создать)
- `src/main.cpp` строки 420-450 (CLI args parsing) — убедиться что --headless работает
- `CMakeLists.txt` — конец файла для добавления custom target

**Файлы для создания:**
- `src/launchers/test_stability.ps1`
- `src/launchers/benchmark_canvas.ps1`
- `src/launchers/check_vram.ps1`
- `src/launchers/run_all_tests.ps1`

**Файлы для редактирования:**
- `CMakeLists.txt` — добавить `run_tests` target

---

## OUTPUT / RETURN

1. Все 4 скрипта созданы в `src/launchers/`
2. `run_all_tests.ps1` вызывает все остальные
3. CMakeLists.txt содержит target `run_tests`
4. `test_stability.ps1` с флагом `--headless` завершается с exit code 0
5. `benchmark_canvas.ps1` выводит таблицу результатов
