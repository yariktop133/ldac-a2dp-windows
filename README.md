# LDAC A2DP source for Windows

Своя замена штатного A2DP-источника Windows (`BthA2dp`): AVDTP-сигналинг, кодирование LDAC (libldac из AOSP) и SBC как запасной вариант.

## Структура

| Путь | Что это |
|---|---|
| `avdtp/` | Ядро протокола на C без кучи и CRT: кадры AVDTP, фрагментация, возможности, LDAC/SBC, автомат источника, медиапакеты. Те же исходники войдут в KMDF-драйвер. |
| `avdtp/tests/` | Юнит-тесты, симулятор наушников (`sim_sink`, повторяет JBL Live 780NC) и повтор реальной сессии (`test_replay`). |
| `driver/LdacA2dp/` | KMDF-драйвер профиля A2DP: L2CAP через BRB, ядро AVDTP, IOCTL для службы кодирования, INF для JBL Live 780NC. |
| `tools/hci/` | Разбор журналов Bluetooth: `btvs` → pcap, ETL → btsnoop, декодер L2CAP/AVDTP/медиа. |
| `tools/ldacenc/` | WAV → LDAC-пакеты A2DP, симуляция радиоканала и ABR. |
| `ui/LdacConfig/` | Утилита настроек (WPF, .NET 10, тема Fluent). |
| `third_party/libldac` | AOSP `external/libldac` (Apache-2.0), коммит `eeee1a3f`. |
| `third_party/compat` | Шимы, чтобы libldac собирался MSVC. |

## Сборка

```bat
build.cmd
```

Собирает C-часть (CMake + Ninja из Visual Studio 2026) и запускает тесты. Утилита настроек:

```bat
dotnet build ui\LdacConfig -c Release
```

Драйвер (нужны WDK/SDK 10.0.28000 и компонент VS «Windows Driver Kit»), результат — `build\LdacA2dp\<Configuration>\LdacA2dp\` (`.sys`, `.inf`, `.cat`, без подписи):

```bat
build_driver.cmd Debug
```

## ldacenc

```bat
build\ldacenc.exe in.wav --quality hq|sq|mq|abr [--mtu 679] [--raw out.ldac] [--packets out.bin] [--link 0:1500,5:550,20:1500]
```

`--link` задаёт пропускную способность канала по времени (секунда:кбит/с); в режиме `abr` глубина очереди передачи управляет адаптивным битрейтом libldac так же, как на Android.

Проверенные результаты (96 кГц / 24 бит, MTU 679): HQ 990, SQ 660, MQ 330 кбит/с; 44.1 кГц SQ 606 кбит/с. ABR: HQ → SQ → Q0 (492) → Q1 (396) → MQ при сужении канала, обратный подъём примерно через 6 с стабильной связи на каждую ступень.

## Статус этапов

- [x] Этап 2: libldac собран под Windows, утилита кодирования, размеры пакетов под MTU.
- [x] Этап 3: логика AVDTP проверена на симуляторе (LDAC, откат на SBC, пиры AVDTP 1.2, конфигурация со стороны наушников, delay report, таймауты).
- [x] Этап 6 (часть): утилита настроек, формат настроек — `docs/settings-format.md`.
- [x] Этап 1: эталонная сессия LDAC с JBL Live 780NC через `btvs` — `docs/jbl-live-780nc.md`.
- [ ] Проверка декодированием: `libldacdec` требует clang (GNU-расширения C); в VS установлены только clang-format/clang-tidy.
- [ ] Этап 4: KMDF-драйвер `driver/LdacA2dp` собирается (Code Analysis и InfVerif чистые), на железе не проверен. Есть: исходящие и входящие (сервер PSM 0x19, фильтр по адресу наушников) каналы AVDTP, асинхронное автоподключение при старте, IOCTL для службы с правами чтения/записи и счётчиками пакетов.
- [ ] Этап 5: звук. Alternative A2DP устроен иначе, чем в нашем плане: `AltA2DP.sys` сам регистрирует KS-интерфейсы Wave/Topology на Bluetooth-устройстве (см. `docs/jbl-live-780nc.md`), без отдельного виртуального устройства.
