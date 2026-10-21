# JBL Live 780NC: эталонная сессия LDAC

Снято 22.10.2026 на этом ноутбуке (Intel Wireless Bluetooth, `USB\VID_8087&PID_0026`): источник — Alternative A2DP Driver 1.8.3.1 (Luculent, служба `AltA2DP`), приёмник — JBL Live 780NC (`12:34:56:78:9A:BC`, DID VID 0x02B0 PID 0x0000).
Инструмент: `btvs.exe` из Bluetooth Test Platform 1.14.0 с Full Packet Logging, поток pcap записан `tools/hci/pcap_tcp_dump.py`, разобран `tools/hci/avdtp_dump.py`. Сырые файлы — `captures/` (не в git: содержат адреса устройств).

## Архитектура драйвера-образца

`AltA2DP` стоит функциональным драйвером на `BTHENUM\{0000110B-0000-1000-8000-00805F9B34FB}_VID&000102B0_PID&0000\…` вместо `BthA2dp` — так же, как планируется наш драйвер (этап 4). HFP (`0000111E`) и AVRCP (`0000110C/110E`) остаются штатными.

## SEP наушников (DISCOVER / GET_ALL_CAPABILITIES)

| SEID | Кодек | Возможности (codec info) |
|---|---|---|
| 1 | SBC | `3f ff 02 35`: 44.1/48 кГц, все режимы каналов, блоки 4–16, 4/8 подполос, SNR/Loudness, bitpool 2–53 |
| 2 | AAC | `80 01 8c 84 00 00`: MPEG-2/4 AAC LC, VBR |
| 3 | LDAC | 44.1 / 48 / 88.2 / 96 кГц; mono / dual / stereo |

Все SEP — аудио-приёмники, у всех есть DELAY_REPORTING. Наушники поддерживают GET_ALL_CAPABILITIES (AVDTP 1.3).

## Последовательность (источник инициирует)

```
> L2CAP ConnReq PSM 0x19 (сигнализация), наушники ConfReq MTU=679
> DISCOVER                                  < 3 SEP
> GET_ALL_CAPABILITIES 1, 2, 3              < SBC, AAC, LDAC
> SET_CONFIGURATION acp=3 int=1  LDAC 96 kHz stereo + DELAY_REPORTING
                                            < ACCEPT
> OPEN 3
                                            < DELAYREPORT 150.0 ms (сразу после конфигурации)
                                            < ACCEPT OPEN
> L2CAP ConnReq PSM 0x19 (медиа), источник ConfReq flush timeout = 96 ms,
  наушники ConfReq MTU=679
  ... ~21 с тишины: START отправляется только когда появляется звук ...
> START 3                                   < DELAYREPORT 150.0 ms, ACCEPT
  медиапоток
> SUSPEND 3 (сразу после окончания звука)   < ACCEPT
```

Байты сигнализации (метки, порядок категорий) совпадают с нашей реализацией побайтно — проверяется тестом `avdtp/tests/test_replay.c`.

## Медиапоток (LDAC SQ, 96 кГц, стерео)

- MTU приёмника 679, пакеты по **673 байта** (12 RTP + 1 заголовок + 660 LDAC), **3 кадра** в пакете, 125 пакетов/с, 674 кбит/с в эфире — то же, что выдаёт наш `ldacenc --quality sq`.
- RTP: V=2, PT=96, **SSRC=0**, seq с 0.
- **Временная метка растёт на 128 за кадр LDAC** (3 кадра → +384) при 96 кГц, хотя кадр содержит 256 сэмплов. `ldacenc` приведён к этому поведению.
- Заголовок первого кадра `aa 73 61 b0`: sync 0xAA, 96 кГц, стерео.

## Что взять в драйвер

- Медиаканал L2CAP: flush timeout 96 мс (устаревшие пакеты выбрасываются контроллером, а не копятся).
- Ждать DELAYREPORT между SET_CONFIGURATION и OPEN — наушники присылают его, не дожидаясь ответа на OPEN.
- START/SUSPEND по наличию звука, а не сразу после OPEN (`auto_start` в `avdtp_source` — для тестов).

## Наблюдения о стеке Windows

- Перезапуск радио Bluetooth через `Windows.Devices.Radios` удалил дочерние устройства JBL (A2DP/HFP/AVRCP) и не пересоздал их: наушники подключались и через 17 с уходили (`Remote User Terminated`). Помог `pnputil /restart-device` адаптера.
- Профиль WPR `BluetoothStack.wprp` пишет только HCI-команды и события; ACL-данные стек отбрасывает, пока в `HKLM\SYSTEM\CurrentControlSet\Services\BthPort\Parameters` не выставлены `EtwDropLargeEvents=0`, `EtwLogSensitiveData=1`, `MaxEtwBytes` (кнопка Full Packet Logging в btvs). После записи эти параметры удалены: `EtwLogSensitiveData` пишет в ETW ключи шифрования.
- `pnputil /restart-device` на A2DP-устройстве — удобный способ заново провести согласование без перезапуска радио.
