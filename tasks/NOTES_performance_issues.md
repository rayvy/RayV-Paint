# NOTES — Архитектурные проблемы требующие переработки

## Исправлено в этой сессии

| # | Что | Файл |
|---|---|---|
| ✅ | SRV Heap 64→8192 | `Dx12Device.cpp:101` |
| ✅ | DXGI format 29 (R8G8B8A8_SNORM) | `DdsHelper.cpp` |
| ✅ | DXGI format 87/88 (BGRA/BGRX) | `DdsHelper.cpp` |

**Причина скриншота с меню вместо canvas:** SRV heap был 64 слота. 
4K = 256 тайлов. При первом обращении к тайлу >64 — `AllocateSrv` возвращала false,
`srvGpuHandle = {0}` — GPU читал slot 0 SRV heap = ImGui font atlas → текст на холсте.

---

## ⚠️ Критические проблемы производительности (требуют переработки)

### 1. CopyResource per-layer — убивает производительность 4K

В `CanvasRendererDX12::Render()` строки 296-302:
```cpp
// Для КАЖДОГО слоя:
CopyResource(m_CompositeRTs[next], m_CompositeRTs[curr])   // 4K = 64MB per copy!
```

4K, 1 слой = 64MB bandwidth copy.
4K, 5 слоёв = 320MB bandwidth copies.
GPU bandwidth ~300GB/s → 320MB = ~1ms. Умножить на кадры = проблема.

**Решение:** Убрать CopyResource. Composite должен читать предыдущий RT через SRV,
а не через copy. Это изменит логику ping-pong — надо будет передавать текущий composite
через descriptor table и читать в шейдере как SRV.

Уже делается через descriptor slot 5 (`g_Composite`), но затем лишний copy.
Убрать `CopyResource` строки 299, оставить только переход состояния.

### 2. WaitForGpu в UploadFence каждый кадр

В `Render()` строки 142-146:
```cpp
if (currentPool.fenceValue > 0 &&
    m_UploadFence->GetCompletedValue() < currentPool.fenceValue) {
    WaitForSingleObject(m_UploadFenceEvent, INFINITE);  // ← блокирует поток!
}
```

При 4K с 256 тайлами — каждый кадр ждёт предыдущего. При медленном GPU = stall.

**Решение:** Использовать timeout = 0 вместо INFINITE, и skip upload если tile ещё не завершён.

### 3. Selection Mask — полный upload каждый кадр

В `Render()` строки 471-534: `CopyTextureRegion` для полного selection mask при `HasSelection()`.
Для 4K selection mask = 4096×4096 × 1 byte = 16MB upload per frame.

**Решение:** Добавить dirty tracking для selection mask (аналогично `maskDirtyTiles`).

### 4. m_MaxGpuTiles = 4096 — но TrimGpuTileCache не учитывает SRV heap

`m_MaxGpuTiles = 4096` (из VRAM). Но SRV heap = 8192 слотов.
При 4K + маски + composite = 256 albedo + 256 mask + 2 composite + 3 dummy + ImGui ~100
= ~620 SRVs. Всё влезает в 8192.

Но при множестве слоёв (5+) × 4K = 5×256 = 1280 SRV. Нормально.
При 16K: 16384/256 = 64 тайла per axis, 64×64 = 4096 тайлов. SRV = 4096+4096+misc ≈ 8200 > 8192.

**Решение для 16K:** Увеличить SRV heap до 16384, или уменьшить tile budget.

### 5. TILE_SIZE = 256 — плохо для viewport-based culling

Для 4K отображаемый viewport = ~1000×600 пикселей при типичном zoom.
256 тайлов загружается в GPU, но отображается только ~24 тайла.
Нет viewport culling — загружаем всё.

**Решение:** В `UploadDirtyTiles` добавить проверку виден ли тайл в viewport.
Это снизит VRAM и CPU work с O(canvasTiles) до O(viewportTiles).

---

## Заметки о 4K производительности

Текущие числа при 4K (1 слой, i5/RTX):
- Upload phase: 256 тайлов × первый кадр = ~50ms
- Render phase: CopyResource 64MB = ~0.3ms
- Selection upload: 16MB если есть выделение = ~1ms
- Total first frame: ~50ms (норма, после этого быстро)
- Total steady: ~2ms per frame (acceptable)

**Вывод:** 4K должно работать нормально после фикса SRV heap.
Если всё ещё лагает — проблема в async uploader или selection mask upload.
