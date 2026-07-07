# TASK 02 — Constant Buffer Ring Overflow Protection

**Файл:** `src/CanvasRendererDX12.cpp` + `src/CanvasRendererDX12.h`
**Приоритет:** ВЫСОКИЙ (возможный out-of-bounds GPU write)
**Сложность:** Низкая

---

## Проблема

В `CanvasRendererDX12`:
```cpp
static constexpr uint32_t MAX_CB_SIZE = 1024 * 1024; // 1 MB ring buffer
uint8_t* m_CbMappedData = nullptr;
uint32_t m_CbOffset = 0;
```

```cpp
uint32_t CanvasRendererDX12::AllocateConstantBufferSpace(const void* data, uint32_t size) {
    // НЕТ ПРОВЕРКИ НА m_CbOffset + size > MAX_CB_SIZE
    uint32_t alignedOffset = (m_CbOffset + 255) & ~255;
    std::memcpy(m_CbMappedData + alignedOffset, data, size);
    m_CbOffset = alignedOffset + size;
    return alignedOffset;
}
```

**Сценарий переполнения:**
- Canvas с 100 слоями × 256 тайлов каждый
- Каждый тайл → один `TileParamsData` (64 bytes aligned to 256) + `LayerBufferData` (64 bytes)
- 100 × 256 × 512 bytes = 12.8MB >> 1MB ring
- Результат: запись за пределы буфера → GPU memory corruption

---

## Решение

### Вариант A: Защита с assert + wrap (простой)

```cpp
uint32_t CanvasRendererDX12::AllocateConstantBufferSpace(const void* data, uint32_t size) {
    uint32_t alignedOffset = (m_CbOffset + 255) & ~255;
    uint32_t requiredEnd = alignedOffset + size;
    
    if (requiredEnd > MAX_CB_SIZE) {
        // Ring wrap: сброс к началу
        // ВАЖНО: только безопасно если GPU уже не читает начало буфера
        Logger::Get().Error("CB ring overflow — wrapping. Consider increasing MAX_CB_SIZE.");
        alignedOffset = 0;
        requiredEnd = size;
        if (requiredEnd > MAX_CB_SIZE) {
            Logger::Get().Error("Single CB allocation exceeds MAX_CB_SIZE — skipping.");
            return 0;
        }
    }
    
    std::memcpy(m_CbMappedData + alignedOffset, data, size);
    m_CbOffset = requiredEnd;
    return alignedOffset;
}
```

### Вариант B: Увеличить размер кольца (немедленное решение)

```cpp
// Изменить с 1MB на 16MB
static constexpr uint32_t MAX_CB_SIZE = 16 * 1024 * 1024; // 16 MB ring buffer
```

16MB достаточно для 100 слоёв × 512 тайлов = 25.6MB при 512b per draw. Нужно 32MB если хотим запас.

**Рекомендация:** Оба варианта. Сначала увеличить до 32MB, добавить проверку с wrap для safety.

---

## Дополнительно — `UpdateCanvasBuffer` и `UpdateLayerBuffer` привязка

Текущий код делает:
```cpp
cmdList->SetGraphicsRootConstantBufferView(0, m_ConstantBufferUpload->GetGPUVirtualAddress() + offset);
```

Это корректно — GPU VA от `GetGPUVirtualAddress()` + offset.
Проверить что offset всегда кратен 256 (D3D12 требует alignment для CBV).

В `AllocateConstantBufferSpace` добавить assert:
```cpp
assert((alignedOffset % 256) == 0 && "CBV must be 256-byte aligned");
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/CanvasRendererDX12.h` — поле `MAX_CB_SIZE`, `m_CbOffset`
- `src/CanvasRendererDX12.cpp` — функция `AllocateConstantBufferSpace` (найти по имени)

**Файлы для редактирования:**
- `src/CanvasRendererDX12.h` — изменить `MAX_CB_SIZE` на `32 * 1024 * 1024`
- `src/CanvasRendererDX12.cpp` — добавить overflow check в `AllocateConstantBufferSpace`

---

## OUTPUT / RETURN

1. `MAX_CB_SIZE = 32 * 1024 * 1024`
2. `AllocateConstantBufferSpace` содержит guard: если `alignedOffset + size > MAX_CB_SIZE` → log error + wrap или skip
3. После wrap `m_CbOffset` сброшен к `size`
4. assert на 256-byte alignment
5. Проект компилируется

---

## Стиль

- `assert()` в debug builds для alignment check
- `Logger::Get().Error()` для runtime overflow (не crash)
- Без изменений в сигнатуре функции
