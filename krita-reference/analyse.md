# Анализ: как Krita открывает 16K, держит стабильность и скорость

Источник: `C:\Users\Rayvy\Documents\GitHub\Krita\`  
Скопированные файлы: этот каталог `krita-reference/`  
Дата: 2026-07-09

---

## Короткий ответ

Krita **не держит 16K-картинку как один плоский буфер**. Она:

1. **Хранит пиксели тайлами 64×64** (sparse hash table + Copy-on-Write).
2. **Сбрасывает «лишние» тайлы на диск** (swap + LZF-сжатие), когда RAM упирается в лимиты.
3. **Обновляет картинку кусками (patches 512×512)** в фоновых потоках (strokes + update queue).
4. **Пока рисуешь — работает на LOD** (уменьшенная копия), а полную детализацию догоняет потом.
5. **На GPU** проекция живёт сеткой OpenGL texture tiles + mipmaps, а не одной 16K-текстурой.

Именно связка «тайлы + swap + async patches + LOD + GPU tiles» даёт 16K без OOM и без «заморозки» UI.

---

## 1. Как Krita открывает 16K изображения

### 1.1. Нет «картинки = vector&lt;pixel&gt;»

Документ — дерево слоёв (`KisImage` → `KisNode` → `KisPaintDevice`).  
У каждого paint device пиксели живут в **`KisTiledDataManager`** (`tiles3/`).

| Параметр | Значение |
|----------|----------|
| Размер тайла (pixel store) | **64 × 64** (`__TILE_DATA_WIDTH/HEIGHT`) |
| RGBA8 тайл | 64×64×4 = **16 KiB** |
| RGBA16F / 16 BPP | 64×64×16 = **64 KiB** |
| Адресное пространство | бесконечное / sparse (hash table) |
| Extent | только реально созданные тайлы |

**16K RGBA8** ≈ 16384×16384×4 ≈ **1 GiB** в плоском виде.  
В Krita это ~256×256 = **65 536 тайлов по 16 KiB** — но:

- пустые области = shared default tile (COW), почти 0 RAM;
- undo хранит только **изменённые** тайлы;
- неиспользуемые / старые тайлы уезжают в swap.

Файлы:

- `tiles3/kis_tiled_data_manager.h` — API хранилища
- `tiles3/kis_tile.h` / `kis_tile_data_interface.h` — тайл + TileData
- `image-core/kis_paint_device.h` — «без фиксированного размера, растёт по итераторам»

### 1.2. Загрузка файла ≠ аллокация 1×гигантского буфера на весь lifetime

Import-плагины (`plugins/impex/*`) читают формат и **пишут байты в paint device** через `writeBytes` / iterators / planar API.  
Данные сразу раскладываются по 64×64 тайлам. После загрузки «плоский» временный буфер импорта (если был) освобождается; рабочая модель — тайловая.

Ключевой инвариант из `KisPaintDevice`:

> A KisPaintDevice doesn't have any fixed size, the size changes dynamically when pixels are accessed by an iterator.

### 1.3. Default pixel + lazy tiles

- Есть **default pixel** (обычно прозрачный).
- `getReadOnlyTileLazy` может отдать shared default, не создавая уникальный тайл.
- Новый тайл создаётся только при **записи** (`getTile(..., writable=true)` → `getTileLazy`).

Итог: открыть 16K «пустой холст» почти бесплатно. Открыть 16K **плотный** PNG всё равно стоит ~1 GiB на слой, но дальше работает swap/pool/COW.

---

## 2. Как Krita остаётся стабильной (не падает / не OOM)

### 2.1. Глобальный `KisTileDataStore` + фоновый swapper

Все `KisTileData` зарегистрированы в **одном** store (`kis_tile_data_store.h`):

```
KisTileDataStore
├── ConcurrentMap tile data
├── KisTileDataPooler   (фон: предклоны для COW)
├── KisTileDataSwapper  (фон: вытеснение в swap)
└── KisSwappedDataStore (диск + компрессия)
```

При создании тайла: `checkFreeMemory()` — если память уже в emergency-зоне, **сразу** запускается swap-job (даже из working thread).

### 2.2. Soft / Hard / Emergency лимиты

Диаграмма из `tiles3/swap/kis_tile_data_swapper_p.h`:

```
  [ OOM ]
  emergencyThreshold  ← новые тайлы ждут, пока не освободим
  hardLimitThreshold  ← aggressive: вытесняем даже working tiles
  hardLimit
  softLimitThreshold  ← soft: только historical/memento (undo) tiles
  softLimit
  [ 0 ]
```

Дефолты (`kis_image_config.cpp`):

| Настройка | Default | Смысл |
|-----------|---------|--------|
| `memoryHardLimitPercent` | **50% RAM** | потолок для tiles |
| `memorySoftLimitPercent` | **2%** от hard | порог «начать выкидывать undo» |
| `memoryPoolLimitPercent` | **0%** | доля hard под COW-pool |
| `maxSwapSize` | **4096 MiB** | размер swap-файла |
| `swapSlabSize` | 64 MiB | slab аллокатора |
| `swapWindowSize` | 16 MiB | mmap-окно в файл |

**Soft pass** (`SoftSwapStrategy`): только `historical()` тайлы (mementoed && ≤1 user) — undo-история.  
**Hard pass** (`AggressiveSwapStrategy`): **любые** тайлы, clock iterator, age-based.

### 2.3. Swap = сжатый temp-файл + memory-mapped окна

- `KisSwappedDataStore`: LZF-компрессия тайла → chunk в allocator → write в `KisMemoryWindow` (QTemporaryFile, окна 16 MiB).
- `KisChunkAllocator`: slab-аллокатор по swap-файлу (default store 4 GiB, slab 64 MiB).
- Состояния TileData: `NORMAL` → `COMPRESSED` → `SWAPPED`.
- Перед чтением: `ensureTileDataLoaded()` + `blockSwapping()` (RW-lock), чтобы swapper не унёс данные из-под ног.

### 2.4. Copy-on-Write + memento (стабильный undo без full-buffer snapshots)

- Несколько `KisTile` могут шарить один `KisTileData` (`m_usersCount`, `m_refCount`).
- Запись → clone tile data (COW). `KisTileDataPooler` **заранее** клонирует горячие тайлы → меньше latency на stroke.
- Undo: `KisMementoManager` регистрирует только **изменённые** тайлы на commit. Rollback/rollforward меняет указатели в hash table, не копирует 16K целиком.
- `purgeHistory` режет старый undo, когда он больше не нужен → free / swap кандидаты.

### 2.5. Age / historical

- `m_age`: 0 = недавно трогали, ≥1 = кандидат на swap.
- Soft strategy сначала markOld, на втором круге — swapOut.
- Historical tiles (только в истории) swap'ятся первыми → рабочий слой остаётся в RAM дольше.

### 2.6. Пулы аллокаций

- Boost `singleton_pool` для 4 BPP / 8 BPP тайлов.
- `SimpleCache` lockless stacks для 4/8/16 BPP — reuse буферов.
- `releaseInternalPools()` при закрытии документа — отдать память ОС.

### 2.7. Что **не** делает RayV-Paint сейчас (и почему 16K болит)

См. `src/core/TileCache.h`:

| RayV-Paint сейчас | Krita |
|-------------------|-------|
| Тайлы **256×256** | **64×64** |
| LRU **удаляет** тайл из RAM (данные **теряются**, если dirty — всё равно evict) | LRU нет; **swap на диск** с восстановлением |
| `m_MaxTilesInRAM = 512` → ~128 MiB RGBA8 и дальше дыра | soft/hard % от **всей** RAM + swap до 4 GiB |
| Нет COW / shared default | shared default + COW + memento |
| Нет фонового swapper | dedicated QThread swapper + pooler |
| Undo = snapshot tile в vector | memento + shared tile data |
| Import: часто flat buffer целиком | writeBytes сразу в tiles |

**Критичный баг-паттерн RayV:** `EvictLRU` просто `erase` — для 16K это либо OOM (если maxTiles большой), либо **потеря пикселей** (если маленький). У Krita evict = swap-out, данные на диске.

---

## 3. Как Krita работает «шустро» на больших изображениях

### 3.1. Partial updates, не full-frame composite

`KisSimpleUpdateQueue` (`update-system/`):

- Большие dirty-rect **режутся на patches** default **512×512** (`updatePatchWidth/Height`).
- Похожие rect'ы **мержатся** (`maxMergeAlpha`, `maxCollectAlpha`).
- Walker обходит только нужные слои/маски в регионе.

Рисуешь кистью 30 px на 16K → композитится не 268 Mpx, а несколько 512-патчей вокруг мазка.

### 3.2. Strokes queue + multi-threaded scheduler

```
UI / tool
   → KisUpdateScheduler
        ├── KisStrokesQueue   (paint ops, undo commands as jobs)
        └── KisSimpleUpdateQueue (projection merges)
              → KisUpdaterContext (N worker threads)
                    → KisAsyncMerger (per-rect composite)
```

- Stroke jobs: exclusive / sequential / barrier / LOD-aware.
- `schedulerBalancingRatio` (default 100): приоритет updates vs strokes.
- UI thread **не** блокируется на full merge; ждёт barrier только когда нужно.

### 3.3. Level of Detail (LOD) — «быстрый отклик кисти»

Файлы: `lod/`, `KisLodPreferences`, `kis_lod_transform.h`, strokes queue LOD factories.

Идея:

- `currentLevelOfDetail() = n` → рабочий масштаб ≈ `2^(-n)`.
- Пока stroke идёт, пишем/композитим **уменьшенную** копию (LodN plane).
- После — sync/regenerate full (Lod0).
- Координаты мазка масштабируются `KisLodTransform`.

На 16K при zoom-out и/или включённом «instant preview» пользователь рисует по ~4K/2K-плоскости — latency как на маленьком холсте.

### 3.4. GPU: texture tiles, не одна 16K texture

`canvas-gpu/`:

- `KisOpenGLImageTextures` — сетка `KisTextureTile` по проекции.
- Upload только dirty image rect → tile update info pool.
- Mipmaps / prepared LOD plane на texture tile (`bindToActiveTexture`).
- `KisImagePyramid` — CPU pyramid для QPainter-fallback / scaled preview.
- `KisCanvasUpdatesCompressor` — не спамить GPU каждым пикселем.

GPU-лимиты (max texture size) обходятся: 16K canvas = много tiles ≤ maxTexSize.

### 3.5. Итераторы и locality

- `kis_hline_iterator` / `kis_vline_iterator` / `kis_random_accessor` — доступ по строкам внутри тайла, кэш-friendly.
- `numContiguousColumns/Rows`, `rowStride` — tight loops без per-pixel hash lookup.
- `bitBlt` / `bitBltRough` — COW share целых тайлов между devices (layer copy, temp devices).

### 3.6. Мелочи производительности

- `getTilesPair` — один fetch old+new для projection (меньше lock pressure).
- Lock-free / concurrent hash (`kis_tile_hash_table2` + `ConcurrentMap`).
- Pre-cloned tile pool (`KisTileDataPooler`) — COW без malloc spike mid-stroke.
- `KisFixedPaintDevice` — маленький contiguous буфер для brush dab (не весь canvas).

---

## 4. Архитектурная схема (end-to-end)

```
[File] ──impex──► writeBytes ──► KisTiledDataManager (64×64 tiles, hash)
                                      │
                                      ├─ KisTile ──COW──► KisTileData
                                      │                      │
                                      │            KisTileDataStore (global)
                                      │              ├ pooler thread
                                      │              └ swapper thread ──► swap file (LZF)
                                      │
                                      └─ KisMementoManager (per-device undo tiles)

[Brush stroke]
   → StrokesQueue job (possibly LodN)
   → paint into device tiles (COW)
   → UpdateQueue patches 512×512
   → AsyncMerger → layer projection tiles
   → OpenGL texture tiles upload (dirty only)
   → Canvas draw with mip/LOD
```

---

## 5. Карта скопированных файлов

```
krita-reference/
├── analyse.md                          ← этот файл
├── tiles3/                             ← pixel store (самое важное)
│   ├── kis_tiled_data_manager.*
│   ├── kis_tile*, kis_tile_data*
│   ├── kis_tile_data_store*, pooler*
│   ├── kis_memento*
│   ├── kis_*_iterator*, random_accessor*
│   ├── kis_tile_hash_table*
│   ├── swap/                           ← disk swap + compression
│   └── tests/kis_low_memory*           ← как тестируют OOM path
├── image-core/                         ← paint device, image, config, merger
├── update-system/                      ← scheduler, strokes, update queue
├── lod/                                ← level-of-detail
└── canvas-gpu/                         ← OpenGL tiles, pyramid, compressor
```

Оригинальные пути:

| Локально | Оригинал |
|----------|----------|
| `tiles3/` | `libs/image/tiles3/` |
| `tiles3/swap/` | `libs/image/tiles3/swap/` |
| `image-core/` | `libs/image/` (выборочно) |
| `update-system/` | `libs/image/` (scheduler/strokes) |
| `lod/` | `libs/image/` |
| `canvas-gpu/` | `libs/ui/opengl/` + `libs/ui/canvas/` |

---

## 6. Что перенять в RayV-Paint (приоритеты)

### P0 — без этого 16K не взлетит стабильно

1. **Disk swap (или memory-mapped spill)** вместо destructive LRU.
   - Dirty/historical tiles → compress → temp file → free RAM.
   - On access → load back.
2. **Memory hard limit % от system RAM**, не fixed `512` tiles.
3. **Не терять dirty tiles** при давлении памяти (сейчас `EvictLRU` опасен).
4. **Import 16K**: stream/chunk write в tiles, **не** держать второй full flat buffer + tiled copy одновременно дольше необходимого.

### P1 — скорость рисования / UI

5. **Partial compose**: dirty rect → patches (256–512), не full canvas blit.
6. **Фоновый worker** для projection/upload (UI thread только submit).
7. **GPU**: atlas/tile textures + upload dirty only (у вас dirty tracking уже есть — довести до end-to-end).
8. **COW tile data** + tile-level undo (не full layer snapshot).

### P2 — «как Krita приятно»

9. **LOD preview** на stroke (½ / ¼ plane).
10. **Shared default tile** для пустых областей.
11. **Smaller tiles (64)** vs 256: лучше granularity undo/swap/dirty; хуже — overhead hash. Krita выбрала 64 осознанно; 128 — разумный компромисс для RayV.
12. **Age-based** eviction (два прохода: mark old → swap), не pure LRU one-shot.

### Размеры (ориентир)

| Canvas | Flat RGBA8 | Tiles 64² | Tiles 256² |
|--------|------------|-----------|------------|
| 4K     | ~64 MiB    | ~16k tiles max | ~1k |
| 8K     | ~256 MiB   | ~65k | ~4k |
| 16K    | ~1 GiB     | ~65k | ~4k |
| 16K × 2 layers + undo | 2–4+ GiB flat | swap обязателен | swap обязателен |

---

## 7. Ключевые файлы — что читать первым

1. `tiles3/swap/kis_tile_data_swapper_p.h` — **диаграмма лимитов** (самое ясное)
2. `tiles3/swap/kis_tile_data_swapper.cpp` — soft/hard pass
3. `tiles3/kis_tile_data_interface.h` — state machine tile data
4. `tiles3/kis_tiled_data_manager.h` — public tile API + memento
5. `tiles3/kis_tile_data_store.h` — global store, memory metric
6. `image-core/kis_image_config.h/.cpp` — все дефолты RAM/swap/patches
7. `update-system/kis_simple_update_queue.h` — patch split/merge
8. `update-system/kis_update_scheduler.h` + `kis_strokes_queue.h` — async model
9. `lod/KisLodPreferences.h` + `kis_lod_transform.h` — LOD
10. `canvas-gpu/kis_opengl_image_textures.h` + `kis_texture_tile.h` — GPU side

---

## 8. Выводы одной фразой

> **Krita масштабируется на 16K, потому что документ — это sparse COW-тайлы с дисковым swap под % RAM, а интерактивность — это patch-updates + multi-thread strokes + LOD preview + tiled GPU textures; а не «загрузить всё в один buffer и надеяться».**

RayV-Paint уже сделал первый шаг (sparse `TileCache` + dirty flags). Следующий качественный скачок — **swap вместо delete-LRU**, **лимиты от RAM**, **patch/async pipeline** и **tile-level COW undo**.
