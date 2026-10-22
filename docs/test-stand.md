# Тестовый стенд для драйвера LdacA2dp

Драйвер ставится **только в виртуальную машину**: ошибка в драйвере ядра даёт синий экран, а `testsigning` ослабляет защиту системы. На рабочем ноутбуке ничего из этого не включаем.

## 1. Железо и ВМ

- **Отдельный USB-адаптер Bluetooth** с BR/EDR и EDR 2/3 Мбит/с (LDAC рассчитан на пакеты 2-DH5): подойдут адаптеры на Intel AX2xx, Realtek RTL8761B/BU. Встроенный Intel ноутбука оставляем хосту.
- **VMware Workstation Pro** (бесплатна для личного использования) или VirtualBox: обе умеют пробрасывать USB-устройство в гостя. Hyper-V для этого не подходит — проброса USB у него нет.
- Гость: Windows 11 x64. Перед каждой установкой драйвера — **снимок ВМ**.

## Как стенд поднят на этом ноутбуке (22.10.2026)

ВМ `LdacTestVM` в VirtualBox, адаптер **Realtek Bluetooth 5.4 (`0BDA:A728`)** проброшен USB-фильтром, учётная запись `tester` (пароль — `btp/vm-credentials.txt`, вне git). Что пришлось обойти:

1. **Hyper-V/VBS на хосте.** VirtualBox поверх Hyper-V (бэкенд NEM) не даёт гостю x2APIC, и Windows 11 25H2 с двумя ядрами не грузится. Отключено: компонент `VirtualMachinePlatform` и `bcdedit /set hypervisorlaunchtype off`, затем перезагрузка (вернуть — `Enable-WindowsOptionalFeature ... VirtualMachinePlatform` и `hypervisorlaunchtype auto`).
2. **VirtualBox 7.2.x + Windows 11 25H2 с ≥2 vCPU:** WinPE и Windows виснут при загрузке на этом ноутбуке (i5-1155G7) при любых настройках: 2–3 ядра, x2APIC, nested paging, `ExitOptimizationEnabled`, 7.2.22 и 7.2.2. С **1 ядром** всё работает — ВМ работает на одном ядре. Стоит VirtualBox **7.2.2**.
3. **Установщик Windows 11 25H2** отказывается ставиться на 1 ядро, `LabConfig` это не обходит. Поэтому Windows развёрнута вручную из WinPE (Shift+F10 на экране установщика): `diskpart` (clean → GPT → EFI 200 МБ → MSR → Windows), `dism /Apply-Image ... /Index:4`, `bcdboot W:\Windows /s S: /f UEFI`, `bcdedit /store ... testsigning on`, копирование `unattend.xml` (учётная запись, автовход, гостевые дополнения при первом входе) со второго виртуального привода (VISO). Разметка, сделанная `diskpart` на хосте, не подошла: `specialize` падал с «File is not system store».
4. Secure Boot выключен (хранилище UEFI без ключей), `testsigning` включён — на рабочем столе «Тестовый режим».

Управление гостем с хоста — `VBoxManage guestcontrol LdacTestVM run --username tester --passwordfile ...`.

## 2. Подготовка гостя (один раз)

1. Пробросить адаптер: VMware → VM → Removable Devices → адаптер → Connect (лучше включить «подключать автоматически»).
2. Поставить драйвер адаптера, проверить в «Диспетчере устройств», что Bluetooth работает.
3. Сопрячь JBL Live 780NC **внутри ВМ** (наушники с multipoint помнят несколько источников). Проверить, что штатный звук через `BthA2dp` работает — это точка отсчёта.
4. В гостевой ОС, от администратора:
   ```bat
   bcdedit /set testsigning on
   ```
5. Отладчик ядра (одно из двух):
   - **по сети (KDNET)**: в госте `kdnet.exe <IP хоста> 50000` (из `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64`, скопировать в гостя вместе с `VerifiedNICList.xml`), команда выдаст ключ; на хосте `windbg -k net:port=50000,key=<ключ>`. У сетевой карты ВМ должен быть эмулируемый Intel e1000e;
   - **через COM-порт**: в VMware добавить Serial Port → Named pipe `\\.\pipe\ldacdbg` (this end is the server, the other end is an application); в госте `bcdedit /debug on` и `bcdedit /dbgsettings serial debugport:1 baudrate:115200`; на хосте `windbg -k com:pipe,port=\\.\pipe\ldacdbg,resets=0,reconnect`.
6. Перезагрузить гостя.

## 3. Сборка и пакет (на хосте)

```bat
build.cmd
build_driver.cmd Debug
powershell -ExecutionPolicy Bypass -File tools\stand\sign-driver.ps1 -Configuration Debug
tools\stand\make-bundle.cmd Debug
```

`sign-driver.ps1` один раз создаёт сертификат `CN=LdacA2dp Test` в хранилище текущего пользователя и подписывает `.sys` и `.cat`. Папку `build\stand` скопировать в гостя.

## 4. Установка в госте

```powershell
.\install-on-vm.ps1            # доверие к сертификату + pnputil /add-driver /install
.\install-on-vm.ps1 -Uninstall # откат
```

Скрипт отказывается работать без включённого `testsigning`. В конце он показывает, какой драйвер обслуживает A2DP-устройство наушников: должна стоять служба `LdacA2dp`. Если осталась `BthA2dp` — переподключить наушники или `pnputil /scan-devices`.

Сразу после установки стоит включить Driver Verifier (и перезагрузиться):

```bat
verifier /standard /driver LdacA2dp.sys
```

## 5. Проверка

1. Драйвер при старте сам согласует поток (как Alternative A2DP): DISCOVER → GET_ALL_CAPABILITIES → SET_CONFIGURATION LDAC 96 кГц → OPEN.
2. Звук пока подаёт служба (своего аудиоустройства у драйвера ещё нет):
   ```bat
   ldacsvc.exe --address 12:34:56:78:9A:BC --source wav:test.wav --sink driver -v
   ```
   или `--source loopback:<имя устройства вывода в ВМ>`. В выводе — согласованный кодек, частота, MTU, задержка; каждую секунду — пакеты/с, битрейт и очередь.
3. Журнал эфира: `btvs.exe -mode wireshark -remote on` в госте + `tools\hci\pcap_tcp_dump.py`, разбор — `tools\hci\avdtp_dump.py`. Сравнивать с эталоном `docs/jbl-live-780nc.md`.
4. Логи WDF: в WinDbg `!wdfkd.wdflogdump LdacA2dp`.

## 6. Что проверить в первую очередь

| Что | Где смотреть | Ожидание |
|---|---|---|
| Сервер L2CAP зарегистрирован | `LDACA2DP_STATUS.ServerRegistered` | 1 |
| MTU медиаканала | `MediaMtu` в статусе / вывод `ldacsvc` | 679 (JBL) |
| Порядок сигнализации | `avdtp_dump.py` | как в эталоне, байт в байт |
| Наушники открывают AVDTP сами | выключить/включить наушники при установленном драйвере | `SignalingIncoming = 1`, поток согласован |
| Потеря медиаканала | отойти с наушниками за предел связи | ABORT, `LastError`, повторное подключение |
| Нагрузка | `ldacsvc` 30+ минут, Driver Verifier | без утечек и багчеков |

## 7. Откат

`install-on-vm.ps1 -Uninstall`, при проблемах — откат снимка ВМ. На хосте после работы можно удалить тестовый сертификат: `certmgr.msc` → Личное → Сертификаты → `LdacA2dp Test`.
