# TASK 12 — Point Sampling для Canvas Viewport (убрать билинейную фильтрацию)

**Файлы:** `src/CanvasRendererDX12.cpp`, `src/shaders/Canvas.hlsl`
**Приоритет:** 🔴 КРИТИЧЕСКИЙ (точность пиксельной работы)
**Сложность:** Низкая (1-2 строки изменений)

---

## Проблема

В `CanvasRendererDX12::CreateRootSignatures()` объявлен static sampler:
```cpp
D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;  // ← БИЛИНЕЙНЫЙ! НЕПРАВИЛЬНО
```

Этот sampler используется **везде** — и для composition, и для viewport отображения.

В растровом редакторе:
- **При отображении пикселей на экран** → `POINT` (nearest neighbor) — иначе пиксели размываются
- **При zoom < 100%** → тоже `POINT` — должны быть видны четкие пиксели
- **При zoom > 100%** → обязательно `POINT` — pixel art / детальная работа требует pixel-perfect

Билинейный фильтр допустим ТОЛЬКО для: resize операции, генерации mipmap, экспорта со сглаживанием.

---

## Решение

### Два sampler'а в root signature

```cpp
// В CreateRootSignatures() — заменить один sampler двумя:

D3D12_STATIC_SAMPLER_DESC samplers[2] = {};

// Sampler 0 (s0) — POINT для canvas viewport
samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;  // ← POINT!
samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
samplers[0].MipLODBias = 0;
samplers[0].MaxAnisotropy = 1;
samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
samplers[0].MinLOD = 0.0f;
samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
samplers[0].ShaderRegister = 0;  // s0
samplers[0].RegisterSpace = 0;
samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

// Sampler 1 (s1) — LINEAR для blend/composite операций (маски, transition)
samplers[1] = samplers[0];
samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
samplers[1].ShaderRegister = 1;  // s1

rootSigDesc.NumStaticSamplers = 2;
rootSigDesc.pStaticSamplers = samplers;
```

### Изменения в Canvas.hlsl

```hlsl
// БЫЛО:
SamplerState g_Sampler : register(s0);

// СТАЛО:
SamplerState g_SamplerPoint  : register(s0);  // Point — для viewport
SamplerState g_SamplerLinear : register(s1);  // Linear — для blend операций
```

Использование:
- `PSMain` (viewport checkerboard): `g_Texture.Sample(g_SamplerPoint, input.uv)` ← POINT
- `PSLayerBlend` (layer composite): `g_Texture.Sample(g_SamplerPoint, uv)` ← POINT
  - Маски тоже: `g_LayerMask.Sample(g_SamplerPoint, uv)`
  - Composite: `g_Composite.Sample(g_SamplerPoint, input.uv)`
- `PSSelectionOutline`: `g_SelectionMask.Sample(g_SamplerPoint, ...)` ← POINT

**Обоснование:** Для composite операций тоже используем POINT — это растровый редактор, каждый пиксель должен быть точным. Билинейное смешивание при layering искажает цвета.

Единственный случай где LINEAR уместен — это checkerboard background при не-integer zoom, но даже там лучше POINT для пиксель-точной работы.

### Изменения в CanvasTiles.hlsl

Проверить и обновить аналогично — если использует `g_Sampler`, заменить на `g_SamplerPoint`.

---

## Также: ImGui Image уже использует Point sampler

`ImGui::Image(...)` по умолчанию использует bilinear для отображения ImGui текстур. Но наш canvas texture уже рендерится через DX12 PSO — ImGui только отображает финальную текстуру `g_canvasTextureSrvGpuHandle` которая уже правильно скомпозирована.

ImGui сам по себе не применяет дополнительную фильтрацию к уже готовой текстуре viewport — она отображается 1:1 в пикселях. Поэтому проблема именно в PSO sampler, а не в ImGui.

---

## INPUT для агента

**Файлы для чтения:**
- `src/CanvasRendererDX12.cpp` — функция `CreateRootSignatures()` (найти по имени, около строки 471 в оригинале, может сместиться)
- `src/shaders/Canvas.hlsl` — строки 29-30 (SamplerState декларация)
- `src/shaders/CanvasTiles.hlsl` — начало файла (sampler декларация)

**Файлы для редактирования:**
- `src/CanvasRendererDX12.cpp` — `CreateRootSignatures()`: заменить 1 sampler на 2
- `src/shaders/Canvas.hlsl` — переименовать sampler + обновить все Sample() вызовы
- `src/shaders/CanvasTiles.hlsl` — аналогично

---

## OUTPUT / RETURN

1. Root signature содержит 2 static samplers: `s0` = POINT, `s1` = LINEAR
2. `Canvas.hlsl` объявляет `g_SamplerPoint : register(s0)` и `g_SamplerLinear : register(s1)`
3. Все `Sample()` в `PSMain`, `PSLayerBlend`, `PSSelectionOutline` используют `g_SamplerPoint`
4. `CanvasTiles.hlsl` обновлён аналогично
5. CSO файлы пересобраны (или runtime compile fallback пересоберёт автоматически)
6. Проект компилируется

---

## Визуальный тест

1. Открыть любую текстуру
2. Zoom in до 400-800% — пиксели должны быть чёткими квадратами, без размытия
3. Zoom out до 25% — пиксели должны быть резкими (nearest neighbor downscale)
4. Провести кистью — след точно на пикселях, без субпиксельного размытия границ

---

## Примечание

После изменения root signature нужно пересобрать CSO:
```
build.bat (пересборка)
```
или удалить `build/Release/bin/shaders/*.cso` чтобы runtime fallback перекомпилировал.
