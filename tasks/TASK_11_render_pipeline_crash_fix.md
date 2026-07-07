# TASK 11 — Render Pipeline: Crash на Resize + Residual Ghosting

**Файлы:** `src/main.cpp`
**Приоритет:** 🔴 КРИТИЧЕСКИЙ
**Сложность:** Средняя

---

## Диагноз — 3 независимых бага в `main.cpp`

### BUG A: Crash при resize окна

`RenderCanvasToTexture()` (строки 1676-1727) содержит:
```cpp
g_DX12.GetCommandAllocator()->Reset()
g_DX12.GetCommandList()->Reset(g_DX12.GetCommandAllocator(), nullptr)
```

Это сбрасывает ТОТ ЖЕ CommandAllocator и CommandList, которые используются в основном render pass чуть позже:
```cpp
// После RenderCanvasToTexture:
if (g_DX12.BeginFrame()) {
    g_DX12.GetCommandList()->...  // ← уже Reset и Close!
```

**Причина краша при resize:**
1. Resize → `g_DX12.ResizeSwapchain()` → `WaitForGpu()` (OK)
2. Следующий кадр: `RenderCanvasToTexture` → `Allocator->Reset()` + `CmdList->Reset()`
3. `CmdList->Close()` → `ExecuteCommandLists()` → `WaitForGpu()`
4. Потом `g_DX12.BeginFrame()` → внутри снова пытается использовать тот же allocator/cmdlist
5. **Double reset / double execute → CRASH**

**Правило DX12**: один CommandAllocator нельзя Reset пока GPU использует его команды. `RenderCanvasToTexture` делает `WaitForGpu()` что правильно, но BeginFrame тоже работает с тем же allocator.

### BUG B: Residual ghosting / следы текстур

`RenderCanvasToTexture` не очищает `g_canvasTexture12` в начале рендера.
`CanvasRendererDX12::Render` переходит в RT состояние и начинает рисовать поверх старых данных.

Первый composite RT очищается (`ClearRenderTargetView` в строке 149 renderer), но `viewportRT` (= `g_canvasTexture12`) — **никогда не очищается явно**. Если canvas пустой или renderer не дошёл до присвоения пикселей — старые данные остаются.

### BUG C: WaitForGpu() каждый кадр = блокирует поток

Строка 1721: `g_DX12.WaitForGpu()` внутри `RenderCanvasToTexture` — вызывается КАЖДЫЙ КАДР.
Это полная CPU-GPU синхронизация, убивает параллелизм. При 4K текстуре = 30-60ms stall.

---

## Решение

### Архитектура: разделить canvas render от swapchain render

Сейчас:
```
RenderCanvasToTexture()  → использует g_DX12.GetCommandList()
BeginFrame()             → использует g_DX12.GetCommandList()  ← КОНФЛИКТ
```

Нужно: canvas рендерится В ТОМ ЖЕ command list что и ImGui, без отдельного Submit.

```cpp
// Правильная последовательность:
g_DX12.BeginFrame()                    // Reset allocator + cmdlist
  → SetDescriptorHeaps
  → RenderCanvasToTexture_Inline()     // записывает в тот же cmdlist
  → ImGui_ImplDX12_RenderDrawData()   // записывает в тот же cmdlist
g_DX12.EndFrame()                      // Close + Execute + Present + Signal
```

---

## Конкретные изменения в main.cpp

### 1. Убрать отдельный Submit из RenderCanvasToTexture

Переименовать в `RecordCanvasCommands(ID3D12GraphicsCommandList* cmdList, int w, int h)`:

```cpp
// БЫЛО: (строки 1676-1727)
void RenderCanvasToTexture(int width, int height) {
    // ...
    GetCommandAllocator()->Reset()   // ← УДАЛИТЬ
    GetCommandList()->Reset(...)     // ← УДАЛИТЬ
    // ...
    GetCommandList()->Close()        // ← УДАЛИТЬ
    ExecuteCommandLists(...)         // ← УДАЛИТЬ
    WaitForGpu()                     // ← УДАЛИТЬ
}

// СТАЛО:
void RecordCanvasCommands(ID3D12GraphicsCommandList* cmdList, int width, int height) {
    if (!g_canvasTexture12 || !g_canvasRtvHeap) return;

    if (g_CanvasRendererReady) {
        bool renderOk = g_CanvasRenderer.Render(
            cmdList, g_Canvas, g_canvasTexture12, g_canvasRtvHandle, width, height
        );
        if (!renderOk) Logger::Get().Error("CanvasRendererDX12::Render failed.");
    } else {
        // Fallback clear
        D3D12_RESOURCE_BARRIER toRT = {};
        toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource = g_canvasTexture12;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &toRT);
        float cc[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
        cmdList->ClearRenderTargetView(g_canvasRtvHandle, cc, 0, nullptr);
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &toRT);
    }
}
```

### 2. Обновить основной render loop (строки ~825-844 и ~1505-1526)

Секция "Draw Canvas Viewport" в main loop:

```cpp
// БЫЛО:
if (viewportWidth > 0 && viewportHeight > 0) {
    ResizeCanvasRenderTarget(viewportWidth, viewportHeight);
    RenderCanvasToTexture(viewportWidth, viewportHeight);  // ← УДАЛИТЬ ВЫЗОВ ЗДЕСЬ
    if (g_canvasTextureSrvGpuHandle.ptr != 0) {
        ImGui::Image(...);
    }
}

// СТАЛО:
if (viewportWidth > 0 && viewportHeight > 0) {
    ResizeCanvasRenderTarget(viewportWidth, viewportHeight);
    // Canvas будет записан в cmdlist ПОСЛЕ ImGui::Render()
    if (g_canvasTextureSrvGpuHandle.ptr != 0) {
        ImGui::Image(...);
    }
}
```

Секция "Standard Render Presentation":

```cpp
// СТАЛО:
ImGui::Render();
if (g_DX12.BeginFrame()) {
    auto* cmdList = g_DX12.GetCommandList();
    ID3D12DescriptorHeap* heaps[] = { g_DX12.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // 1. Canvas render (записать команды)
    if (viewportWidth > 0 && viewportHeight > 0) {
        RecordCanvasCommands(cmdList, viewportWidth, viewportHeight);
    }

    // 2. Main RT → ImGui
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_DX12.GetCurrentRtv();
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    HRESULT hr = g_DX12.EndFrame();
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HandleDeviceLost(g_hWnd);
    }
}
```

### 3. Fix B: Явная очистка canvas RT при изменении размера

В `ResizeCanvasRenderTarget()` после создания ресурса добавить initial clear через временный cmdlist:

В `CanvasRendererDX12::Render()` — в самом начале после перехода viewportRT в RT состояние — добавить `ClearRenderTargetView`:

```cpp
// В CanvasRendererDX12::Render(), после:
TransitionResource(cmdList, viewportRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

// ДОБАВИТЬ:
float zeroClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
cmdList->ClearRenderTargetView(viewportRtv, zeroClear, 0, nullptr);
// (это устраняет ghosting)
```

### 4. Fix C: Убрать WaitForGpu из per-frame пути

`RenderCanvasToTexture` → `RecordCanvasCommands` больше не вызывает WaitForGpu.
`g_DX12.EndFrame()` уже сигнализирует fence — GPU sync произойдёт в следующем `BeginFrame()`.

---

## Сохранить для тестового пути

Тестовый путь (строки ~1484-1502) использует отдельный `BeginFrame()` + `EndFrame()`.
Для него canvas render не нужен — оставить как есть.

---

## INPUT для агента

**Файлы для чтения:**
- `src/main.cpp` строки 825-844 (viewport canvas draw)
- `src/main.cpp` строки 1505-1526 (main render presentation)
- `src/main.cpp` строки 1580-1727 (ResizeCanvasRenderTarget + CleanupCanvasRenderTarget + RenderCanvasToTexture)
- `src/CanvasRendererDX12.cpp` строки 80-337 (Render function начало)

**Файлы для редактирования:**
- `src/main.cpp`
- `src/CanvasRendererDX12.cpp` (добавить clear viewportRT)

---

## OUTPUT / RETURN

1. `RenderCanvasToTexture` переименована в `RecordCanvasCommands(cmdList, w, h)` без Submit/Execute/Wait
2. Canvas команды записываются В ТОМ ЖЕ cmdlist что и ImGui
3. Порядок в кадре: BeginFrame → RecordCanvas → clear main RT → ImGui → EndFrame
4. `CanvasRendererDX12::Render()` очищает viewportRT в начале перед composite
5. Нет `WaitForGpu()` в per-frame render пути (только в resize)
6. Resize окна НЕ крашится
7. Нет остаточных следов от предыдущих текстур

---

## Тест

Запустить приложение:
1. Открыть любой файл
2. Resize окно несколько раз быстро → не должно крашиться
3. Открыть другой файл → нет следов от предыдущего
4. FPS должен вырасти (нет per-frame WaitForGpu)
