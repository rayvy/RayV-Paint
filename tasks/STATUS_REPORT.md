# RayVPaint — Lead Maintainer Status Report
> Дата: 2026-07-07 | Ревьюер: Lead AI Maintainer

---

## 1. Вектор проекта — оценка

**C++20 + DirectX 12 — верное решение.** Аргументация:

| Критерий | Оценка |
|---|---|
| Контроль над GPU pipeline | ✅ DX12 даёт полный контроль над командными очередями, барьерами, хипами |
| Производительность на тяжёлых файлах | ✅ Sparse tiles + async upload — единственный путь к 8K/16K без заморозок |
| C++20 | ✅ `std::span`, `std::optional`, `std::filesystem`, structured bindings — уже используются |
| TileCache архитектура | ✅ Правильная идея. 256×256 тайлы = sparse memory. RGBA8 tile = 256KB, RGBA32F = 1MB |
| ImGui + GLFW | ✅ Минималистичный UI слой без лишних зависимостей |

---

## 2. Текущее состояние — что сделано

### ✅ Выполнено хорошо
- **DX11 полностью удалён** — Gate 1 пройден (0 хитов на d3d11/D3D11/ID3D11)
- **Canvas.h чист** — нет D3D11 включений, Gate 2 пройден
- **TileCache** — sparse LRU, RGBA8/RGBA32F, dirty tracking
- **CanvasRendererDX12** — ping-pong composite RT, per-tile GPU textures, layer blend PSO
- **Shader pipeline** — CSO precompile через fxc + runtime fallback + embedded HLSL
- **PaintEngine** — CPU brush stamping с TileCache
- **UndoRedoManager** — tile-level delta snapshots
- **Async copy queue** — Dx12AsyncUploader for uploading dirty tiles on a background copy queue

### ⚠️ Выполнено криво

1. **main.cpp — монолит 1917 строк**
   - Весь рендер, swapchain, descriptor heap — в одном файле
   - ~20 голых глобальных COM-указателей без RAII (`g_pd3d12Device`, `g_pSwapChain`, etc.)
   - Нет класса-обёртки для DX12 device/swapchain

2. **Staging resources — потенциальный GPU crash**
   - `m_StagingResources.clear()` в начале кадра освобождает буферы предыдущего frame
   - GPU ещё может читать эти буферы → undefined behavior / device lost
   - Нужен per-frame staging pool с fence guard

3. **Mask upload — нет dirty tracking**
   - Mask tiles перезаписываются каждый кадр для всех albedo tiles
   - Нет флага `maskDirty` per-tile → overupload

4. **CB ring — нет защиты от переполнения**
   - `m_CbOffset` сбрасывается в 0 каждый кадр, но нет guard при превышении 1MB в рамках одного кадра
   - Много слоёв + много тайлов = потенциальный out-of-bounds write

5. **Selection mask upload — блокирует рендер**
   - Monolithic R8 texture + синхронный `CopyTextureRegion` каждый frame при наличии selection
   - Для 8K: 8MB upload = ~5-10ms GPU stall

6. **Floating pixels — flat float vector**
   - `m_FloatingPixels` в Canvas = `std::vector<float>` на весь слой
   - 8K × 8K × 4 float = 1GB (!) → OOM

7. **GetCompositePixels() — блокирующая**
   - Синхронная операция: полный обход всех тайлов с alpha blend на CPU
   - Вызывается при каждом Copy и Export

8. **Magic Wand / Bucket Fill — ✅ Оптимизировано и Перенесено в background/GPU**
   - Фоновые потоки через ThreadPool с поддержкой отмены и тайловым кэш-дружелюбным доступом.
   - Добавлен вспомогательный класс GpuComputeTools и шейдер FloodFill.hlsl для GPU-ускоренных вычислений.

### ❌ Не реализовано (из плана Phase 7-8)
- `src/render/Dx12Device.h/.cpp` — device изолирован в main.cpp
- `src/render/Dx12Descriptors.h/.cpp` — heap управление в main.cpp
- GPU compute brush / blur / HSV
- Tiled selection mask
- VRAM overflow / eviction policy
- Canvas.cpp не разбит на субмодули

---

## 3. Архитектурные вопросы — честные ответы

### Сможет ли пользователь без лагов работать с 8K/16K?

**Текущий ответ: нет.** Вот почему:

| Операция | Проблема при 8K |
|---|---|
| Magic Wand | CPU flood fill, 64M пикселей, ~500ms+ |
| Bucket Fill | Аналогично |
| Blur / HSV / Curves | 256MB alloc + полный CPU обход |
| Move Pixels | `m_FloatingPixels` flat float = 1GB при 8K |
| Selection mask GPU upload | 8MB синхронно каждый frame |
| Composite export | 256MB blocking alloc |
| Dirty tile upload | Синхронно в main command list, stall |

### Что произойдёт если закончится VRAM?

- TileCache имеет LRU eviction на CPU стороне (`m_MaxTilesInRAM = 512`)
- Но GPU tiles никогда не эвиктируются — растут до исчерпания VRAM
- Device Lost → crash (нет recovery)

### Что нужно для настоящей производительности на 8K

1. **Tiled selection mask** — заменить `std::vector<uint8_t>` на sparse структуру
2. **Floating pixels через TileCache** — убрать flat vector
3. **Async copy queue** — `D3D12_COMMAND_LIST_TYPE_COPY` в отдельном потоке
4. **GPU tile eviction** — LRU для GPU side, upload on-demand при рендере
5. **Magic Wand GPU compute** — BFS compute shader (multi-pass), results в R8 texture
6. **Tiled CPU operations** — Blur/HSV/Curves работают по тайлам с border padding
7. **Device Lost recovery** — поймать `DXGI_ERROR_DEVICE_REMOVED`, пересоздать device

---

## 4. Задачные файлы

Созданы в `tasks/`:
- `TASK_01_staging_fence_guard.md` — исправить staging lifetime
- `TASK_02_cb_ring_overflow.md` — защита CB ring
- `TASK_03_mask_dirty_tracking.md` — mask upload оптимизация
- `TASK_04_dx12_device_class.md` — вынести device в класс
- `TASK_05_dx12_descriptors_class.md` — вынести descriptor heap
- `TASK_06_canvas_split.md` — разбить Canvas.cpp
- `TASK_07_floating_tilecache.md` — floating pixels → TileCache
- `TASK_08_selection_mask_tiled.md` — tiled selection mask
- `TASK_09_async_upload_queue.md` — async copy command list
- `TASK_10_gpu_compute_tools.md` — GPU compute для тяжёлых операций
