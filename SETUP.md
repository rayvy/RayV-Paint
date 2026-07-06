# RayVPaint Setup

## Requirements

- Windows 10 or 11, 64-bit
- Visual Studio 2022 with the "Desktop development with C++" workload
- Git
- CMake 3.20+

## Build

Run:

```cmd
build.bat
```

This configures CMake in `build/` and builds the release binaries.

## Test

Run:

```cmd
run_tests.bat
```

This launches the executable in `--test` mode and exits after one frame.

## Notes

- The project downloads third-party dependencies with CMake `FetchContent`.
- No Python runtime is required.
