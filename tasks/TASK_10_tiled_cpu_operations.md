# TASK 10 — Tiled CPU Operations: Blur / HSV / Curves / Noise

**Файлы:** `src/Canvas.cpp` (или `src/CanvasAdjustments.cpp` после TASK_05)
**Приоритет:** СРЕДНИЙ (нужен для 8K без OOM)
**Сложность:** Средняя
**Зависимость:** TASK_05 (Canvas split)

---

## Проблема

Текущие destructive операции (`ApplyBlur`, `ApplyHSV`, `ApplyCurves`, `ApplyNoise`) вероятно работают через:
```cpp
// Экспортировать весь canvas в flat buffer
std::vector<float> pixels = ExportLayerF(layer);  // 8K = 1GB!!!
// ... обработать
// Импортировать обратно
SetLayerPixelsF(layer, pixels);
```

Или через `GetCompositePixels()` / аналоги — плоский буфер всего canvas.

Для 8K canvas RGBA32F: 8192 × 8192 × 16 bytes = **1 Гигабайт** alloc.
Это неприемлемо.

---

## Целевая архитектура: Tiled Processing

Все операции должны работать **по тайлам** через `TileCache` API.

### Принцип

```cpp
// Вместо:
std::vector<float> pixels(width * height * 4);
ExportAll(pixels);
ProcessAll(pixels);
ImportAll(pixels);

// Используем:
for (каждый тайл в TileCache) {
    uint8_t* tile = LockTile(tx, ty);      // Прямой доступ к тайлу
    ProcessTile(tile, tx, ty);             // Обработать на месте
    MarkDirty(tx, ty);                     // GPU upload в следующем frame
}
```

### Граничные условия (border padding)

Для `ApplyBlur` — нужны пиксели из соседних тайлов (border = radius пикселей):

```cpp
// Для тайла (tx, ty) при blur radius R:
// Нужно прочитать область [tx*TILE_SIZE - R, ty*TILE_SIZE - R]
//                       до [(tx+1)*TILE_SIZE + R, (ty+1)*TILE_SIZE + R]

// Создать локальный буфер с padding:
int padded_size = TILE_SIZE + 2 * R;
std::vector<float> padded(padded_size * padded_size * 4);

// Заполнить из TileCache::GetPixelF() для каждого пикселя border
// (GetPixelF обрабатывает out-of-bounds как transparent)
for (int py = -R; py < TILE_SIZE + R; ++py) {
    for (int px = -R; px < TILE_SIZE + R; ++px) {
        int canvasX = tx * TILE_SIZE + px;
        int canvasY = ty * TILE_SIZE + py;
        float rgba[4] = {};
        tileCache.GetPixelF(canvasX, canvasY, rgba);  // clamp/transparent OOB
        int pidx = ((py + R) * padded_size + (px + R)) * 4;
        padded[pidx + 0] = rgba[0];
        padded[pidx + 1] = rgba[1];
        padded[pidx + 2] = rgba[2];
        padded[pidx + 3] = rgba[3];
    }
}

// Применить blur к центральному TILE_SIZE×TILE_SIZE региону
// Записать результат в tileCache через LockTile()
```

---

## Реализация каждой операции

### ApplyHSV (без border padding — per-pixel, нет зависимости от соседей)

```cpp
void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx < 0) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.tileCache) return;
    
    auto& tc = *layer.tileCache;
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();
    
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (!tc.HasTile(tx, ty)) continue;
            
            uint8_t* tileData = tc.LockTile(tx, ty);
            if (!tileData) continue;
            
            // Обработать каждый пиксель тайла in-place
            for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                // Проверить selection mask
                int canvasX = tx * TILE_SIZE + (i % TILE_SIZE);
                int canvasY = ty * TILE_SIZE + (i / TILE_SIZE);
                if (m_HasSelection && m_SelectionMask[canvasY * m_Width + canvasX] == 0)
                    continue;
                
                float r, g, b, a;
                if (bpp == 4) {  // RGBA8
                    r = tileData[i*4 + 0] / 255.f;
                    g = tileData[i*4 + 1] / 255.f;
                    b = tileData[i*4 + 2] / 255.f;
                    a = tileData[i*4 + 3] / 255.f;
                } else {  // RGBA32F
                    float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                    r = fp[0]; g = fp[1]; b = fp[2]; a = fp[3];
                }
                
                // RGB → HSV → adjust → RGB
                auto [h, s, v] = RGBtoHSV(r, g, b);
                h = std::fmod(h + dH + 360.f, 360.f);
                s = std::clamp(s + dS, 0.f, 1.f);
                v = std::clamp(v + dV, 0.f, 1.f);
                auto [nr, ng, nb] = HSVtoRGB(h, s, v);
                
                if (bpp == 4) {
                    tileData[i*4 + 0] = static_cast<uint8_t>(nr * 255.f + 0.5f);
                    tileData[i*4 + 1] = static_cast<uint8_t>(ng * 255.f + 0.5f);
                    tileData[i*4 + 2] = static_cast<uint8_t>(nb * 255.f + 0.5f);
                    // alpha без изменений
                } else {
                    float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                    fp[0] = nr; fp[1] = ng; fp[2] = nb;
                }
            }
            tc.MarkDirty(tx, ty);
        }
    }
    layer.needsUpload = true;
    MarkCompositeDirty();
    
    // Undo: сохранить snapshot ПЕРЕД операцией (нужно вызывать до изменений)
}
```

### ApplyBlur (с border padding)

```cpp
void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx < 0) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.tileCache) return;
    
    auto& tc = *layer.tileCache;
    int R = static_cast<int>(std::ceil(radius));
    
    // Создать копию TileCache для чтения (чтобы не читать изменённые пиксели)
    // Альтернатива: separable blur — сначала horizontal pass в temp, потом vertical
    // Рекомендуется separable: O(n*R) вместо O(n*R^2)
    
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    
    // Horizontal pass → temp TileCache
    TileCache tempCache;
    tempCache.Init(m_Width, m_Height, tc.GetFormat());
    
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (!tc.HasTile(tx, ty)) continue;
            
            for (int py = 0; py < TILE_SIZE; ++py) {
                for (int px = 0; px < TILE_SIZE; ++px) {
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasX >= m_Width || canvasY >= m_Height) continue;
                    
                    if (m_HasSelection && m_SelectionMask[canvasY * m_Width + canvasX] == 0) {
                        // Копировать без blur
                        float rgba[4]; tc.GetPixelF(canvasX, canvasY, rgba);
                        tempCache.SetPixelF(canvasX, canvasY, rgba);
                        continue;
                    }
                    
                    float sum[4] = {};
                    float weight = 0;
                    for (int k = -R; k <= R; ++k) {
                        float w = std::exp(-k*k / (2 * radius * radius));
                        float rgba[4]; tc.GetPixelF(canvasX + k, canvasY, rgba);
                        sum[0] += rgba[0]*w; sum[1] += rgba[1]*w;
                        sum[2] += rgba[2]*w; sum[3] += rgba[3]*w;
                        weight += w;
                    }
                    float result[4] = {sum[0]/weight, sum[1]/weight, sum[2]/weight, sum[3]/weight};
                    tempCache.SetPixelF(canvasX, canvasY, result);
                }
            }
        }
    }
    
    // Vertical pass: tempCache → tc
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (!tc.HasTile(tx, ty)) continue;
            uint8_t* tileData = tc.LockTile(tx, ty);
            for (int py = 0; py < TILE_SIZE; ++py) {
                for (int px = 0; px < TILE_SIZE; ++px) {
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasX >= m_Width || canvasY >= m_Height) continue;
                    
                    if (m_HasSelection && m_SelectionMask[canvasY * m_Width + canvasX] == 0) continue;
                    
                    float sum[4] = {};
                    float weight = 0;
                    for (int k = -R; k <= R; ++k) {
                        float w = std::exp(-k*k / (2 * radius * radius));
                        float rgba[4]; tempCache.GetPixelF(canvasX, canvasY + k, rgba);
                        sum[0] += rgba[0]*w; sum[1] += rgba[1]*w;
                        sum[2] += rgba[2]*w; sum[3] += rgba[3]*w;
                        weight += w;
                    }
                    float result[4] = {sum[0]/weight, sum[1]/weight, sum[2]/weight, sum[3]/weight};
                    // SetPixelF на LockTile data (или через tileCache API)
                    tc.SetPixelF(canvasX, canvasY, result);
                }
            }
            tc.MarkDirty(tx, ty);
        }
    }
    
    layer.needsUpload = true;
    MarkCompositeDirty();
}
```

### ApplyCurves (per-pixel LUT — no border)

Аналогично HSV, только вместо HSV конвертации:
```cpp
// Применить LUT к каждому каналу R, G, B
auto applyLut = [&](float v) -> float {
    int idx = std::clamp(static_cast<int>(v * 255.f), 0, 255);
    return lut256[idx];
};
nr = applyLut(r); ng = applyLut(g); nb = applyLut(b);
```

### ApplyNoise (per-pixel — no border)

```cpp
// На месте: добавить random noise
float noise = ((float)rand() / RAND_MAX - 0.5f) * 2.f * strength;
if (colorNoise) {
    nr = std::clamp(r + ((float)rand()/RAND_MAX - 0.5f) * 2.f * strength, 0.f, 1.f);
    // ... разный noise для G, B
} else {
    nr = ng = nb = std::clamp(r + noise, 0.f, 1.f);  // grey noise
}
```

---

## Parallelization через ThreadPool

Для больших canvas — параллелить по строкам тайлов:

```cpp
// Parallel tiles processing
std::atomic<int> nextTileRow{0};
int totalRows = tc.GetTilesY();
int numWorkers = std::min((int)std::thread::hardware_concurrency(), totalRows);

std::vector<std::future<void>> futures;
for (int w = 0; w < numWorkers; ++w) {
    futures.push_back(ThreadPool::Get().Submit([&]() {
        int ty;
        while ((ty = nextTileRow.fetch_add(1)) < totalRows) {
            for (int tx = 0; tx < tc.GetTilesX(); ++tx) {
                ProcessTile(tx, ty);
            }
        }
    }));
}
for (auto& f : futures) f.wait();
```

**Важно:** `LockTile` должен быть thread-safe (каждый тайл модифицируется только одним потоком при параллельном row processing).

---

## INPUT для агента

**Файлы для чтения:**
- `src/core/TileCache.h` — полностью (методы: GetPixelF, SetPixelF, LockTile, HasTile, GetTilesX/Y, MarkDirty)
- `src/Canvas.cpp` — `ApplyBlur`, `ApplyHSV`, `ApplyCurves`, `ApplyNoise`, `InvertAlpha` (найти по имени)
- `src/core/ThreadPool.h` — API

**Файлы для редактирования:**
- `src/Canvas.cpp` или `src/CanvasAdjustments.cpp` (после TASK_05) — переписать все Apply* функции

---

## OUTPUT / RETURN

1. `ApplyHSV` работает per-tile без плоского буфера
2. `ApplyBlur` работает с separable pass + border от GetPixelF
3. `ApplyCurves` работает per-tile с LUT
4. `ApplyNoise` работает per-tile
5. `InvertAlpha` работает per-tile
6. Все операции уважают `m_HasSelection` и `m_SelectionMask`
7. После операции `MarkDirty` на каждый изменённый тайл + `layer.needsUpload = true`
8. Нет `std::vector<float>` размером с весь canvas
9. Проект компилируется

---

## Стиль

- Обрабатывать только `HasTile() == true` тайлы (sparse)
- `LockTile` создаёт тайл если не существует → использовать только когда пишем
- `GetPixelF` для чтения (безопасен для OOB — возвращает transparent)
- Undo: сохранять snapshot ДО операции через `BackupTile(tx, ty)` для каждого affected tile
- Не забыть `SetDocumentModified(true)` после успешной операции
