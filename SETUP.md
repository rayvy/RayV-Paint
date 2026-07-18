# Инструкция по настройке окружения и сборке RayVPaint

Эта инструкция поможет вам настроить рабочее окружение с нуля (например, после переустановки Windows) и запустить сборку проекта.

---

## 1. Требования к системе и ПО

Для сборки и запуска проекта вам понадобятся:

1. **Операционная система:** Windows 10 или 11 (64-bit).
2. **Visual Studio 2022:**
   - Скачайте бесплатную версию **Visual Studio 2022 Community** с официального сайта Microsoft.
   - **Важно:** При установке в инсталляторе Visual Studio обязательно отметьте галочкой рабочую нагрузку **«Разработка классических приложений на C++»** (Desktop development with C++). Это автоматически установит:
     - Компилятор MSVC (C++)
     - Windows SDK (необходим для работы с DirectX 11)
     - CMake (встроенный в Visual Studio)
3. **Python 3.13:**
   - Скачайте и установите **Python 3.13** (или новее) с официального сайта python.org.
   - **Важно:** При установке отметьте галочку **"Add python.exe to PATH"** (Добавить Python в переменные среды).
4. **Git:**
   - Скачайте и установите **Git для Windows**, чтобы скачивать зависимости. Должен быть доступен из командной строки.

---

## 2. Зависимости проекта (Управление библиотеками)

Проект использует следующие внешние библиотеки:
- **GLFW** (создание окон, контекст OpenGL/DirectX, обработка ввода).
- **Dear ImGui** (интерфейс редактора, ветка `docking`).

> [!NOTE]
> Вам **не нужно** скачивать эти библиотеки вручную. CMake настроен через механизм `FetchContent` — при первой сборке он сам скачает нужные версии библиотек с GitHub и скомпилирует их вместе с проектом.

---

## 3. Сборка проекта

Для сборки проекта в корне репозитория созданы два скрипта автоматизации:

### Вариант А: Быстрая сборка через командную строку (рекомендуется)
Просто запустите файл `build.bat` двойным кликом или из терминала:
```cmd
build.bat
```
Этот скрипт:
1. Найдет установленную Visual Studio 2022.
2. Подключит переменные окружения компилятора (`vcvarsall.bat`).
3. Сгенерирует проект CMake в папке `build/`.
4. Скомпилирует релизную версию приложения в `build/bin/Release/RayVPaint.exe`.

### Вариант Б: Открытие в Visual Studio 2022
1. Запустите Visual Studio 2022.
2. Выберите **«Открыть локальную папку»** (Open a local folder) и укажите корень репозитория `RayV-Paint`.
3. Visual Studio автоматически распознает файл `CMakeLists.txt`, скачает зависимости и настроит проект.
4. Вы можете запускать и отлаживать проект клавишей **F5**.

---

## 4. Автоматические тесты (Autotests)

Для проверки работоспособности без ручного запуска интерфейса приложение поддерживает тестовый режим:
```cmd
build/Release/RayVPaint.exe --test
```
В этом режиме программа:
- Создаст окно приложения.
- Инициализирует устройство DirectX 11 и Swap Chain.
- Проверит инициализацию интерфейса Dear ImGui.
- Выполнит один кадр отрисовки.
- Завершится с кодом `0` в случае успеха или с кодом `1` при ошибке.

Скрипт `run_tests.bat` собирает проект и прогоняет выбранный suite:

```cmd
run_tests.bat              :: smoke (default): --test + test_script.py
run_tests.bat smoke
run_tests.bat unusual      :: edge cases / exotic DDS / unicode / OOB (test_unusual_scenarios.py)
run_tests.bat 16k          :: heavy 16K open (test_16k.py)
run_tests.bat stress       :: hellish 16K reliability (fill + undo thrash, hang gate 2s)
run_tests.bat all          :: smoke → unusual → 16k → stress
```

Headless Python scripts / stress:
```cmd
build\Release\RayVPaint.exe --headless --script test_script.py
build\Release\RayVPaint.exe --headless --script test_unusual_scenarios.py
build\Release\RayVPaint.exe --test-16k
build\Release\RayVPaint.exe --stress-16k
```

Emergency reliability plan: `plans/EMERGENCY_SAFE_PLAN.MD`  
Stress exit codes: `0` ok · `1` hang >2s · `2` undo leak · `3` missing asset · `4` load fail  

**Stress live journal** (flushed after every action; crash appends here too):
- `testfield/stress_journal.txt` (cwd)
- `Documents/RayVPaint/user/stress_last.txt` (stable copy)

In stress mode **CrashGuard is silent** (no blocking MessageBox). If the process dies, open the journal — the last lines are the crash report.

