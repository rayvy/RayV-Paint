# DirectXTex (library-only for RayVPaint)

**Do not vendor tools** (Texconv / Texassemble / DDSView).

## Source of truth

Implementation files live in the repo checkout:

```
DirectXTex-main/DirectXTex/
```

This directory only provides a **minimal CMake target** (`DirectXTex` static lib) configured for RayVPaint:

| Option | Value |
|--------|--------|
| Tools / samples | off |
| D3D11 GPU BC compress | **off** (v1) |
| D3D12 | off |
| BC OpenMP | **on** (if found) |
| Xbox / EXR / JPEG / PNG helpers | off |

## Syncing upstream

Replace or update `DirectXTex-main/` from [Microsoft/DirectXTex](https://github.com/microsoft/DirectXTex), then rebuild. No need to copy sources into `third_party/` unless you want a fully self-contained tree without `DirectXTex-main/`.

## License

MIT — see `DirectXTex-main/LICENSE`.
