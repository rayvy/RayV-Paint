Окей, это классическая задача — Windows Explorer генерирует превью не через ассоциацию "программа по умолчанию", а через отдельный **Thumbnail Handler** (COM-объект), который регистрируется для расширения файла. Просто назначив свой .exe как программу по умолчанию, ты получаешь только иконку — превью это отдельный контракт.

## Как это устроено

**1. Интерфейс `IThumbnailProvider`**

Начиная с Vista, Explorer использует COM-интерфейс `IThumbnailProvider::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)`. Твой обработчик получает поток файла (через `IInitializeWithStream` или `IInitializeWithFile`), декодирует DDS-заголовок, достаёт первый мип нулевого уровня (или генерит превью так же, как это делает твой основной вьюпорт), и отдаёт готовый `HBITMAP` нужного размера `cx x cx`.

Минимальный набор интерфейсов, которые нужно реализовать в COM-объекте:
- `IInitializeWithStream` (или `IInitializeWithFile`) — Explorer передаёт тебе файл
- `IThumbnailProvider` — собственно генерация битмапа
- `IUnknown` / `IClassFactory` — стандартная COM-обвязка

**2. Регистрация в реестре**

Это отдельная DLL (не твой основной .exe, COM in-proc сервер), которую регистрируешь так:

```
HKEY_CLASSES_ROOT\.dds\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}
    (Default) = "{ТВОЙ-CLSID}"

HKEY_CLASSES_ROOT\CLSID\{ТВОЙ-CLSID}
    (Default) = "RayVPaint DDS Thumbnail Handler"
    InprocServer32\(Default) = "путь\к\dll"
    InprocServer32\ThreadingModel = "Apartment"
```

GUID `{e357fccd-a995-4576-b01f-234630154e96}` — это фиксированный, зарезервированный Microsoft GUID именно для thumbnail handler'ов, его не меняешь.

**3. Регистрация самой DLL**

DLL должна экспортировать `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer` — стандартный ATL/COM-сервер. Регистрируется через `regsvr32 yourhandler.dll`.

Если не хочешь городить ATL руками — можно взять минимальный шаблон COM in-proc сервера, реализация `IThumbnailProvider` там буквально одна функция на 20 строк, весь остальной код — boilerplate регистрации.

**4. Сброс кэша превьюшек**

Explorer жёстко кэширует превью. После регистрации новые иконки не появятся, пока не:
- очистишь кэш: `ie4uinit.exe -show` или удалить `%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db` (Explorer нужно перезапустить)
- либо программно поднять версию через `HKCR\.dds` → добавить/сменить произвольный маркер, но по факту проще всего дёрнуть `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL)` после установки/апдейта

## Практический путь

Раз у тебя уже есть DX11-пайплайн для чтения DDS и рендера превью в самом RayVPaint — вынеси кусок "декодировать DDS → получить RGBA буфер нужного маленького размера" в отдельную библиотеку/функцию, которую будет дёргать и основное приложение, и thumbnail-handler DLL. Не обязательно гонять полный DX11 контекст ради превью — для маленьких размеров (обычно Explorer просит 32/96/256 px) дешевле сделать software-decode DDS (BC1-7, DXT и т.п.) в CPU и просто собрать HBITMAP через GDI, без GPU-контекста — это быстрее и не тянет девайс-контекст в explorer.exe процесс, что важно, потому что thumbnail handler грузится **внутри explorer.exe** (или в изолированном surrogate-процессе `dllhost.exe`, если поставить флаг `DisableProcessIsolation = 0`, что желательно для стабильности).

Хочешь, накидаю скелет C++ COM-объекта (IInitializeWithStream + IThumbnailProvider + регистрация) под твой конкретный DDS-ридер?