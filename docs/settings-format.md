# Формат настроек и статуса

Утилита `LdacConfig` пишет, служба кодирования (этап 5) читает.

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
      "adaptive": true
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

Утилита не даёт снять последний флажок в группе, поэтому пустых масок в файле не бывает.

## `%ProgramData%\LdacA2dp\status\<AA-BB-CC-DD-EE-FF>.json`

Пишет служба во время воспроизведения, утилита опрашивает раз в секунду и показывает в разделе «Текущее состояние»:

```json
{ "codec": "LDAC", "channelMode": "Stereo", "sampleRate": 96000, "bitDepth": 24, "bitrateKbps": 660 }
```
