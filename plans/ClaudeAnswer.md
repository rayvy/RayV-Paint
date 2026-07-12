## Что реализовать

Property Handler — COM-объект, отдельный от Thumbnail Handler, отвечает за метаданные в колонках Explorer, панели "Подробности" и tooltip (InfoTip). Показывает DXGI Format, Width, Height, MipCount, ArraySize, IsCubemap без открытия файла.

Компоненты:
1. `.propdesc` XML-схема — объявляет кастомные PROPERTYKEY (formatID = свой GUID, propID 2+)
2. COM DLL, реализующая `IPropertyStore` + `IInitializeWithStream`
3. Парсер DDS header (128 байт + опционально 20 байт DX10 extension) — без загрузки пикселей
4. Регистрация в реестре

## Чеклист проверки (от вероятного к менее вероятному)

**Схема свойств**
- Проверь `.propdesc` зарегистрирован через `PSRegisterPropertySchema()` без ошибки
- Проверь formatID GUID уникальный, не конфликтует с другими схемами
- Проверь propID начинается с 2 (0 и 1 зарезервированы)

**Реестр — привязка хендлера**
- Проверь `HKCR\.dds\PropertyHandler\(Default)` = точный CLSID
- Проверь `HKCR\CLSID\{GUID}\InprocServer32\(Default)` = абсолютный путь к DLL
- Проверь `ThreadingModel = Apartment`

**COM-реализация**
- Проверь `IInitializeWithStream::Initialize` не падает на битых/неполных DDS
- Проверь `GetValue()` возвращает `S_OK` + корректный `PROPVARIANT` тип (совпадает с `.propdesc`)
- Проверь `GetValue()` для отсутствующих/неприменимых свойств возвращает пустой PROPVARIANT, не падает
- Проверь `GetCount`/`GetAt` (если IPropertyStoreCapabilities) корректны

**DXGI Format парсинг**
- Проверь legacy DDS (без DX10 header) — маппинг из `fourCC` (DXT1/3/5 и т.д.)
- Проверь DX10 extended header — `dxgiFormat` читается напрямую
- Проверь fallback на "Unknown" вместо краша при неизвестном формате

**Видимость в Explorer**
- Проверь колонка добавлена через ПКМ на заголовке списка → "Подробнее"
- Проверь `InfoTip` строка в `HKCR\.dds` = `"prop:Prop1;Prop2;..."` для тултипа
- Проверь имена свойств в InfoTip совпадают с `name=` в `.propdesc` один-в-один

**Права/архитектура**
- Проверь regsvr32/установка от админа (HKCR requires elevation)
- Проверь DLL x64 (не x86) под 64-бит Explorer
- Проверь HKCU\Software\Classes не переопределяет HKCR веткой с другим/старым CLSID

**Кэш**
- Проверь Explorer перезапущен после регистрации (property system кэширует schema)
- Проверь смена propID/formatID у существующего свойства = требует новый GUID, иначе бита кэш схемы

**Диагностика**
- Проверь через `ShellExView` (NirSoft) — виден ли Property Handler, не disabled
- Проверь через `sdkddkver`/Property System viewer (`Microsoft PowerToys` PropertyEdit или `PropSys` test tools) — значения реально отдаются