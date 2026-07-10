# Исследование буферов и структуры экспорта XXMI/3DMigoto

В данном документе описана структура конфигурационных файлов `.ini` и бинарных буферов (`.buf` / `.ib`), создаваемых инструментами импорта/экспорта XXMI для игр HoYoverse (в частности, Zenless Zone Zero, Genshin Impact, Honkai Star Rail). 

Документ составлен для интеграции парсера в 3D-вьювер/редактор.

---

## 1. Ссылки на файлы и строки кода (Пруфы)

### A. Хранение нормалей контура (Outline Normals)
Логика усреднения и упаковки нормалей контура находится в файле:
`C:\Users\Rayvy\AppData\Roaming\Blender Foundation\Blender\5.0\scripts\addons\XXMITools\migoto\exporter.py`

*   **Zenless Zone Zero (ZZZ):**
    *   *Строки 619–648:* Вычисляется вектор нормали аутлайна (`verts_outline_vector`), проецируется на тангент (`tan`) и битангент (`bitan`) меша. Проекции записываются в семантику `TEXCOORD1` (в буфер `Texcoord.buf`):
        ```python
        dot_prods[:, 0] = numpy.einsum("ij,ij->i", tan, verts_outline_vector)
        dot_prods[:, 1] = numpy.einsum("ij,ij->i", bitan, verts_outline_vector) + 1
        dot_prods[:, 1] *= -1.0
        dot_prods[:, 1] += 1.0  # Равно -dot(bitan, normal)
        ```
*   **Genshin Impact / Honkai Star Rail / Honkai Impact 3rd:**
    *   *Строки 581–599:* Вектор записывается напрямую в `TANGENT.xyz` буфера позиций (`Position.buf`).
*   **Honkai Impact 3rd Part 2:**
    *   *Строки 600–618:* Вектор записывается в RGB-каналы цвета вершин `COLOR.rgb` (буфер `Position.buf`).

### B. Логика разделения буферов при экспорте (Posed vs Static)
Логика разделения меша на три кастомных буфера при наличии скелета/анимации находится в файлах:
1.  `C:\Users\Rayvy\AppData\Roaming\Blender Foundation\Blender\5.0\scripts\addons\XXMITools\migoto\data\data_model.py`
    *   *Строки 488–500:* Инициализация структуры буферов: `"Position"`, `"Blend"` (веса и индексы костей) и `"TexCoord"`.
    *   *Строки 552–600:* Семантики `Position`, `Normal` и `Tangent` распределяются в буфер `"Position"`. Семантики `Blendweight` и `Blendindices` уходят в `"Blend"`. Семантики `TexCoord` и `Color` уходят в `"TexCoord"`.
2.  `C:\Users\Rayvy\AppData\Roaming\Blender Foundation\Blender\5.0\scripts\addons\XXMITools\migoto\exporter.py`
    *   *Строки 388–403:* Если `component.blend_vb != ""` (есть привязка к костям), на диск записываются три отдельных файла: `<Name>Position.buf`, `<Name>Blend.buf` и `<Name>Texcoord.buf`.

### C. Исходная структура дампов (3DMigoto Frame Analysis)
Исходная разметка вершинного буфера `vb0` до разделения (показывает оригинальные сдвиги байт и типы данных) взята из файла:
`E:\MEGA\Lewd Modding Team\Rayvy\Belle Ultimate\Belle Project\DUMP 3.0\AfterClean\Belle\BelleHairA-vb0=71d2bf80.txt`
*   *Строки 5–76 (Элементы разметки):*
    *   `element[0]: POSITION` (R32G32B32_FLOAT, Offset: 0) -> 12 байт
    *   `element[1]: NORMAL` (R32G32B32_FLOAT, Offset: 12) -> 12 байт
    *   `element[2]: TANGENT` (R32G32B32A32_FLOAT, Offset: 24) -> 16 байт
    *   `element[3]: BLENDWEIGHTS` (R32G32B32A32_FLOAT, Offset: 40) -> 16 байт
    *   `element[4]: BLENDINDICES` (R32G32B32A32_UINT, Offset: 56) -> 16 байт
    *   `element[5]: COLOR` (R8G8B8A8_UNORM, Offset: 72) -> 4 байта
    *   `element[6]: TEXCOORD` (R16G16_FLOAT, Offset: 76) -> 4 байта (базовый UV)
    *   `element[7]: TEXCOORD1` (R32G32_FLOAT, Offset: 80) -> 8 байт (проекция аутлайна)
    *   `element[8]: TEXCOORD2` (R16G16_FLOAT, Offset: 88) -> 4 байта (карта света)

---

## 2. Спецификация структуры экспорта

### A. Файл ИНИ (`*.ini`)
Пример: `G:\XXMI\ZZMI\Mods\@LewdLadBelleUltimate\Belle.ini`

*   **Ресурсы (`[Resource...]`):** Описывают имя бинарного файла и его шаг (`stride`).
    *   `stride = 40` для Position
    *   `stride = 32` для Blend
    *   `stride = 20` или `24` для Texcoord
    *   `format = DXGI_FORMAT_R32_UINT` для индексных буферов (`*.ib`).
*   **Связывание буферов (`[TextureOverride...]`):** Связывает оригинальный хеш буфера из игры с нашими кастомными ресурсами через слоты D3D11 (`vb0`, `vb1`, `vb2`).
*   **Отрисовка и Сборка (`drawindexed`):** Секции оверрайдов частей (например, `[TextureOverrideBelleBodyA]`) содержат команды вида:
    `drawindexed = <index_count>, <index_offset>, 0`
    Индексный буфер читается последовательно, выделяя треугольники для каждого меша согласно этим смещениям.

### B. Бинарные буферы меша

1.  **Позиционный буфер (`*Position.buf`, шаг 40 байт):**
    ```cpp
    struct PositionVertex {
        float position[3];  // 12 байт (координаты X, Y, Z)
        float normal[3];    // 12 байт (нормали)
        float tangent[4];   // 16 байт (тангент XYZ и знак бинормали W)
    };
    ```
2.  **Текстурный буфер (`*Texcoord.buf`, шаг 20 или 24 байта):**
    *   При шаге **20**:
        ```cpp
        struct TexcoordVertex20 {
            uint32_t color;      // 4 байта (R8G8B8A8_UNORM)
            uint16_t uv0[2];     // 4 байта (half2: базовый UV)
            float uv1[2];        // 8 байт (float2: проекция нормали контура в ZZZ)
            uint16_t uv2[2];     // 4 байта (half2: UV2 карты света)
        };
        ```
    *   При шаге **24** добавляется:
        ```cpp
        uint16_t uv3[2];         // 4 байта (half2: дополнительный UV3)
        ```
3.  **Индексный буфер (`*.ib`):**
    Массив `uint32_t` индексов, сгруппированных по 3 для каждого треугольника.

---

## 3. Ограничения реализации для 3D-вьювера RayV-Paint

1.  **Скининг и Блендинг не нужны:** Поскольку вьювер предназначен только для предпросмотра текстур и базовой геометрии, информация из буфера костей (`*Blend.buf`) и весов полностью игнорируется. Задача воссоздать полный скелетный рендер или анимацию отсутствует.
2.  **Чтение «как есть» без модификации ресурсов:** Редактор никак не изменяет и не переписывает бинарные файлы `.buf` / `.ib` или текстуры `.dds`. Они считываются исключительно для рендеринга геометрии. Любое редактирование в RayV-Paint касается только 2D-текстур.
3.  **Параметры Constant Buffer (CB):** Логика константных буферов (свет, матрицы трансформации движка игры) не воссоздается напрямую. Матрицы вида, проекции, базовое освещение и смещения модели хардкодятся и передаются из параметров 3D-окна редактора для свободного вращения/масштабирования меша.
