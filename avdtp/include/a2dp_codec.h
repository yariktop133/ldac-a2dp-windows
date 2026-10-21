/*
 * A2DP codec information elements: SBC (mandatory) and LDAC (vendor 0x012D/0x00AA),
 * plus selection of a configuration from local and remote capabilities.
 */
#ifndef A2DP_CODEC_H
#define A2DP_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2DP_CODEC_SBC 0x00
#define A2DP_CODEC_VENDOR 0xFF

#define LDAC_VENDOR_ID 0x0000012Du
#define LDAC_CODEC_ID 0x00AAu
#define LDAC_INFO_LEN 8 /* vendor(4) codec(2) freq(1) channel mode(1) */

/* LDAC sampling frequency bits */
#define LDAC_FREQ_44100 0x20
#define LDAC_FREQ_48000 0x10
#define LDAC_FREQ_88200 0x08
#define LDAC_FREQ_96000 0x04
#define LDAC_FREQ_176400 0x02
#define LDAC_FREQ_192000 0x01
/* LDAC channel mode bits (same values as LDACBT_CHANNEL_MODE_*) */
#define LDAC_CM_MONO 0x04
#define LDAC_CM_DUAL 0x02
#define LDAC_CM_STEREO 0x01

#define SBC_INFO_LEN 4
#define SBC_FREQ_16000 0x80
#define SBC_FREQ_32000 0x40
#define SBC_FREQ_44100 0x20
#define SBC_FREQ_48000 0x10
#define SBC_CM_MONO 0x08
#define SBC_CM_DUAL 0x04
#define SBC_CM_STEREO 0x02
#define SBC_CM_JOINT 0x01
#define SBC_BLOCK_4 0x80
#define SBC_BLOCK_8 0x40
#define SBC_BLOCK_12 0x20
#define SBC_BLOCK_16 0x10
#define SBC_SUBBANDS_4 0x08
#define SBC_SUBBANDS_8 0x04
#define SBC_ALLOC_SNR 0x02
#define SBC_ALLOC_LOUDNESS 0x01
#define SBC_BITPOOL_HQ 53

/* Capability masks (several bits set) or a configuration (one bit per field). */
typedef struct a2dp_ldac {
    uint8_t freq;
    uint8_t cm;
} a2dp_ldac;

typedef struct a2dp_sbc {
    uint8_t freq;
    uint8_t cm;
    uint8_t block;
    uint8_t subbands;
    uint8_t alloc;
    uint8_t min_bitpool;
    uint8_t max_bitpool;
} a2dp_sbc;

typedef enum a2dp_codec_id { A2DP_SEL_NONE = 0, A2DP_SEL_SBC, A2DP_SEL_LDAC } a2dp_codec_id;

typedef struct a2dp_config {
    a2dp_codec_id codec;
    uint32_t sample_rate;
    uint8_t channels;
    a2dp_ldac ldac;
    a2dp_sbc sbc;
} a2dp_config;

/* User preferences, mirrored from the settings UI. Masks restrict what the
 * source is allowed to negotiate; preferred rate wins if both sides allow it. */
typedef struct a2dp_prefs {
    uint8_t allow_ldac;
    uint8_t ldac_freq_mask;
    uint8_t ldac_cm_mask;
    uint32_t ldac_pref_rate;
    uint32_t sbc_pref_rate;
} a2dp_prefs;

void a2dp_prefs_default(a2dp_prefs *p);

int a2dp_ldac_parse(uint8_t codec_type, const uint8_t *info, size_t len, a2dp_ldac *out);
size_t a2dp_ldac_build(const a2dp_ldac *c, uint8_t *out, size_t cap);
int a2dp_sbc_parse(uint8_t codec_type, const uint8_t *info, size_t len, a2dp_sbc *out);
size_t a2dp_sbc_build(const a2dp_sbc *c, uint8_t *out, size_t cap);

uint32_t a2dp_ldac_freq_to_rate(uint8_t bit);
uint8_t a2dp_ldac_rate_to_freq(uint32_t rate);

/* Pick one configuration out of the intersection. Return 0 on success,
 * an A2DP/AVDTP error code if there is no common configuration. */
int a2dp_select_ldac(const a2dp_ldac *local, const a2dp_ldac *peer, uint32_t pref_rate, a2dp_config *out);
int a2dp_select_sbc(const a2dp_sbc *local, const a2dp_sbc *peer, uint32_t pref_rate, a2dp_config *out);

/* Checks that a configuration proposed by the peer is a single valid choice within `local`. */
int a2dp_check_ldac_config(const a2dp_ldac *local, const a2dp_ldac *cfg, a2dp_config *out);
int a2dp_check_sbc_config(const a2dp_sbc *local, const a2dp_sbc *cfg, a2dp_config *out);

#ifdef __cplusplus
}
#endif

#endif /* A2DP_CODEC_H */
