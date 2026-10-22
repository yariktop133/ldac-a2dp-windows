# Формат настроек и статуса

Утилита `LdacConfig` пишет, служба кодирования `LdacSvc` читает и применяет изменения на лету (проверяет время записи файла раз в секунду).

## `%ProgramData%\LdacA2dp\settings.json`

```json
{
  "devices": {
    "66:77:88:99:AA:BB": {
      "driver": "Ldac",
      "codec": "LDAC",
      "allowMono": false,
      "allowStereo": true,
      "rate44": true, "rate48": true, "rate88": true, "rate96": true,
      "bits16": true, "bits24": true,
      "channelMode": "Stereo",
      "quality": "SQ",
      "adaptive": true,
      "holdSeconds": 3
    }
  }
}
```

Соответствие `a2dp_prefs` (`avdtp/include/a2dp_codec.h`):

| Поле | `a2dp_prefs` / кодировщик |
|---|---|
| `codec` = `SBC` | `allow_ldac = 0` |
| `rate44/48/88/96` | `ldac_freq_mask` (`LDAC_FREQ_*`); `ldac_pref_rate` = наибольшая разрешённая |
| `allowMono`, `allowStereo` + `channelMode` | `ldac_cm_mask`: Stereo → `LDAC_CM_STEREO`, Dual Channel → `LDAC_CM_DUAL`, Моно → `LDAC_CM_MONO` |
| `bits16/24` | формат PCM на входе `ldacBT_init_handle_encode` (`S16`/`S24`) |
| `quality` | `LDACBT_EQMID_MQ/SQ/HQ`, меняется на лету через `ldacBT_set_eqmid` |
| `adaptive` | `ldac_ABR_Proc(..., flagEnable = 1)` |
| `holdSeconds` | сколько секунд держать поток в START после последнего звука, если поток Windows уже закрыт (1–600, по умолчанию 3). Пока поток Windows открыт, передача держится и без этого |

Утилита не даёт снять последний флажок в группе, поэтому пустых масок в файле не бывает.

## `%ProgramData%\LdacA2dp\status\<AA-BB-CC-DD-EE-FF>.json`

Служба пишет его раз в секунду, пока обслуживает наушники, и удаляет в конце сессии. Утилита опрашивает файл раз в секунду и показывает в разделе «Текущее состояние». Если файл старше 5 с, утилита считает, что данных нет.

```json
{ "codec": "SBC", "channelMode": "Joint Stereo", "sampleRate": 48000, "bitDepth": 16, "bitrateKbps": 357,
  "streaming": false, "quality": "bp53", "droppedPackets": 0,
  "avrcp": "absoluteVolume", "absoluteVolume": true, "volumeDb": -17.8, "muted": false }
```

| Поле | Значение |
|---|---|
| `streaming` | поток в START (идёт звук или тишина в паузе), иначе SUSPEND |
| `quality` | текущий шаг кодера: для LDAC `HQ`/`SQ`/`Q0`/`Q1`/`MQ` (ABR), для SBC `bp<bitpool>` |
| `droppedPackets` | пакеты, которые служба не смогла поставить в очередь драйвера за сессию |
| `avrcp` | `none` (нет AVRCP-устройства), `closed`, `opening`, `connected`, `absoluteVolume`, `noAbsoluteVolume` |
| `absoluteVolume`, `volumeDb`, `muted` | громкость конечной точки и кто её применяет: наушники (AVRCP) или служба |

Состояние самой службы (работает, остановлена, не установлена) утилита берёт у диспетчера служб.
