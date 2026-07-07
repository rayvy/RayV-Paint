# RayVPaint — Architecture Master Document
> Lead Maintainer Reference | 2026-07-07

## Порядок выполнения задач

```
Приоритет 1 (Стабилизация — делать сразу):
  TASK_01 → TASK_02 → TASK_03

Приоритет 2 (Архитектура — параллельно или после P1):
  TASK_04 → TASK_05

Приоритет 3 (Performance — после P2):
  TASK_06 → TASK_07 → TASK_08 → TASK_09 → TASK_10
```

## Зависимости

```
TASK_01 (staging fence)    ← независим
TASK_02 (cb ring)          ← независим
TASK_03 (mask dirty)       ← независим
TASK_04 (Dx12Device)       ← независим
TASK_05 (canvas split)     ← независим
TASK_06 (floating tiles)   ← после TASK_05
TASK_07 (async upload)     ← после TASK_01, TASK_04
TASK_08 (gpu compute)      ← после TASK_05
TASK_09 (vram/device lost) ← после TASK_04
TASK_10 (tiled CPU ops)    ← после TASK_05
```

## Файловая карта проекта

```
src/
├── main.cpp                        [1917 ln] СОКРАТИТЬ → после TASK_04
├── Canvas.h                        [349 ln]  ✅ ЧИСТ — GPU API не используется
├── Canvas.cpp                      [~2500 ln] РАЗБИТЬ → TASK_05
├── CanvasLayers.cpp                [NEW] ← TASK_05
├── CanvasSelection.cpp             [NEW] ← TASK_05
├── CanvasPaint.cpp                 [NEW] ← TASK_05
├── CanvasAdjustments.cpp          [NEW] ← TASK_05, TASK_10
├── CanvasTransforms.cpp            [NEW] ← TASK_05
├── CanvasFileIO.cpp                [NEW] ← TASK_05
├── CanvasUndo.cpp                  [NEW] ← TASK_05
├── CanvasRendererDX12.h            [204 ln]  рефактор TASK_01,02,03,07
├── CanvasRendererDX12.cpp          [1319 ln] рефактор TASK_01,02,03,07
├── render/
│   ├── Dx12Device.h/.cpp           [NEW] ← TASK_04
│   └── Dx12AsyncUploader.h/.cpp   [NEW] ← TASK_07
├── core/
│   ├── TileCache.h/.cpp            ✅ ХОРОШИЙ КОД — без изменений
│   ├── PaintEngine.h/.cpp          ✅ без изменений
│   ├── UndoRedoManager.h/.cpp      ✅ без изменений
│   ├── ThreadPool.h/.cpp           ✅ без изменений
│   ├── Logger.h/.cpp               ✅ без изменений
│   ├── DdsHelper.h/.cpp            ✅ без изменений
│   ├── ImageManager.h/.cpp         ✅ без изменений
│   ├── ClipboardHelper.h/.cpp      ✅ без изменений
│   ├── ConfigManager.h/.cpp        ✅ без изменений
│   ├── KeymapManager.h/.cpp        ✅ без изменений
│   └── TexconvHelper.h/.cpp        ✅ без изменений
└── shaders/
    ├── Canvas.hlsl                  ✅ без изменений
    └── CanvasTiles.hlsl             ✅ без изменений
```

## Концептуальная архитектура для 8K/16K

```
┌─────────────────────────────────────────────────────────────────┐
│                         main.cpp                                │
│  [Orchestrator: GLFW, ImGui, Dx12Device, CanvasRenderer, Canvas]│
└──────────────────┬──────────────────────────────────────────────┘
                   │
        ┌──────────▼──────────┐
        │    Dx12Device       │ (TASK_04)
        │  Device, Swapchain  │
        │  Descriptor Heap    │
        │  Frame sync         │
        └──────────┬──────────┘
                   │ device*, queue*, allocSrv/FreeSrv
        ┌──────────▼──────────────────────────────────┐
        │          CanvasRendererDX12                  │
        │  ┌──────────────┐  ┌──────────────────────┐ │
        │  │ PSO: checker │  │ PSO: layer blend      │ │
        │  │ PSO: outline │  │ PSO: tile blend       │ │
        │  └──────────────┘  └──────────────────────┘ │
        │  ┌──────────────────────────────────────────┐│
        │  │  GPU Tile Cache                          ││
        │  │  map<TileCache*, LayerGpuResources>      ││
        │  │  LRU eviction (TASK_09)                  ││
        │  └──────────────────────────────────────────┘│
        │  ┌──────────────┐  ┌──────────────────────┐ │
        │  │ Async Upload │  │ Frame Staging Pool    │ │
        │  │ Copy Queue   │  │ Fence-guarded         │ │
        │  │ (TASK_07)    │  │ (TASK_01)             │ │
        │  └──────────────┘  └──────────────────────┘ │
        └──────────────────────────────────────────────┘
                   ↑ reads dirty tiles from
        ┌──────────────────────────────────────────────┐
        │              Canvas (Document Model)          │
        │                                              │
        │  Layer[]                                     │
        │  ├── tileCache: TileCache (sparse, LRU)     │
        │  ├── filteredCache: TileCache               │
        │  ├── mask: vector<uint8_t> → TILED (TASK_08)│
        │  └── needsUpload, maskNeedsUpload flags      │
        │                                              │
        │  m_FloatingTileCache: TileCache (TASK_06)   │
        │  m_SelectionMask: vector<uint8_t>           │
        │                                              │
        │  Operations (tiled after TASK_10):           │
        │  ├── ApplyBlur (separable, border pad)      │
        │  ├── ApplyHSV (per-tile, per-pixel)         │
        │  ├── ApplyCurves (per-tile LUT)             │
        │  ├── MagicWand (BFS in ThreadPool - TASK_08)│
        │  └── BucketFill (async BFS - TASK_08)       │
        └──────────────────────────────────────────────┘
                   │ reads from
        ┌──────────▼──────────────────────────────────┐
        │              TileCache (Core)                │
        │  Sparse: only allocated tiles in RAM         │
        │  LRU: evict least-recently-used              │
        │  Formats: RGBA8 (4B/px), RGBA32F (16B/px)  │
        │  Tile size: 256×256 px                       │
        │  8K canvas: 32×32 = 1024 tiles max          │
        │  RGBA8 all tiles: 256MB, RGBA32F: 1GB       │
        └─────────────────────────────────────────────┘
```

## Ответы на концептуальные вопросы

### Тайловая система — нужна ли?
**Да, и она уже есть.** TileCache = sparse 256×256 tiles. 
Добавить нужно только: tiled operations (TASK_10) и tiled selection mask.

### Система масок — как улучшить?
- Сейчас: flat `vector<uint8_t>` = 8K→64MB, 16K→256MB
- После TASK_03: dirty tracking, не перезаливать каждый frame
- В будущем: sparse mask TileCache для экономии памяти

### Magic Wand — молниеносный?
- Сейчас: CPU BFS, однопоточный
- После TASK_08: ThreadPool BFS + cache-friendly tile access
- Прирост: 4-8× на современных CPU (8-16 cores)
- Для истинного GPU: Jump Flooding Algorithm в compute shader (Phase 8+)

### 8K/16K без лагов — реально?
| Разрешение | После всех задач | Ограничения |
|---|---|---|
| 4K (4096×4096) | ✅ Полностью работает | Нет |
| 8K (8192×8192) | ✅ Brush/Paint | Magic Wand ~100ms |
| 8K | ✅ Blur/HSV | Первый кадр после import медленный |
| 16K (16384×16384) | ⚠️ Paint OK | Composite RT = 1GB VRAM |
| 16K | ❌ Magic Wand | CPU BFS = 268M итераций |

Для истинного 16K без лагов нужен Phase 8 (GPU compute brush + GPU flood fill).

### Защита от крашей?
- TASK_01: staging fence → нет GPU memory corruption
- TASK_09: device lost recovery → emergency autosave + recovery
- TileCache LRU: CPU side не OOM при умеренном документе
- TASK_09: GPU tile eviction → VRAM не переполняется
