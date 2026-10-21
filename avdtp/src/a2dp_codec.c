#include "a2dp_codec.h"
#include "avdtp.h"

#include <string.h>

#define ONE_BIT(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)

void a2dp_prefs_default(a2dp_prefs *p)
{
    memset(p, 0, sizeof(*p));
    p->allow_ldac = 1;
    p->ldac_freq_mask = LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000;
    p->ldac_cm_mask = LDAC_CM_STEREO | LDAC_CM_DUAL | LDAC_CM_MONO;
    p->ldac_pref_rate = 96000;
    p->sbc_pref_rate = 48000;
}

static const struct { uint8_t bit; uint32_t rate; } ldac_rates[] = {
    { LDAC_FREQ_44100, 44100 }, { LDAC_FREQ_48000, 48000 },   { LDAC_FREQ_88200, 88200 },
    { LDAC_FREQ_96000, 96000 }, { LDAC_FREQ_176400, 176400 }, { LDAC_FREQ_192000, 192000 },
};

static const struct { uint8_t bit; uint32_t rate; } sbc_rates[] = {
    { SBC_FREQ_16000, 16000 }, { SBC_FREQ_32000, 32000 },
    { SBC_FREQ_44100, 44100 }, { SBC_FREQ_48000, 48000 },
};

uint32_t a2dp_ldac_freq_to_rate(uint8_t bit)
{
    size_t i;
    for (i = 0; i < sizeof(ldac_rates) / sizeof(ldac_rates[0]); i++)
        if (ldac_rates[i].bit == bit)
            return ldac_rates[i].rate;
    return 0;
}

uint8_t a2dp_ldac_rate_to_freq(uint32_t rate)
{
    size_t i;
    for (i = 0; i < sizeof(ldac_rates) / sizeof(ldac_rates[0]); i++)
        if (ldac_rates[i].rate == rate)
            return ldac_rates[i].bit;
    return 0;
}

static uint32_t sbc_freq_to_rate(uint8_t bit)
{
    size_t i;
    for (i = 0; i < sizeof(sbc_rates) / sizeof(sbc_rates[0]); i++)
        if (sbc_rates[i].bit == bit)
            return sbc_rates[i].rate;
    return 0;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int a2dp_ldac_parse(uint8_t codec_type, const uint8_t *info, size_t len, a2dp_ldac *out)
{
    if (codec_type != A2DP_CODEC_VENDOR || len < LDAC_INFO_LEN)
        return -1;
    if (rd32le(info) != LDAC_VENDOR_ID || (uint16_t)(info[4] | info[5] << 8) != LDAC_CODEC_ID)
        return -1;
    out->freq = info[6] & 0x3F;
    out->cm = info[7] & 0x07;
    return 0;
}

size_t a2dp_ldac_build(const a2dp_ldac *c, uint8_t *out, size_t cap)
{
    if (cap < LDAC_INFO_LEN)
        return 0;
    out[0] = (uint8_t)(LDAC_VENDOR_ID & 0xFF);
    out[1] = (uint8_t)(LDAC_VENDOR_ID >> 8 & 0xFF);
    out[2] = (uint8_t)(LDAC_VENDOR_ID >> 16 & 0xFF);
    out[3] = (uint8_t)(LDAC_VENDOR_ID >> 24 & 0xFF);
    out[4] = (uint8_t)(LDAC_CODEC_ID & 0xFF);
    out[5] = (uint8_t)(LDAC_CODEC_ID >> 8);
    out[6] = c->freq & 0x3F;
    out[7] = c->cm & 0x07;
    return LDAC_INFO_LEN;
}

int a2dp_sbc_parse(uint8_t codec_type, const uint8_t *info, size_t len, a2dp_sbc *out)
{
    if (codec_type != A2DP_CODEC_SBC || len < SBC_INFO_LEN)
        return -1;
    out->freq = info[0] & 0xF0;
    out->cm = info[0] & 0x0F;
    out->block = info[1] & 0xF0;
    out->subbands = info[1] & 0x0C;
    out->alloc = info[1] & 0x03;
    out->min_bitpool = info[2];
    out->max_bitpool = info[3];
    return 0;
}

size_t a2dp_sbc_build(const a2dp_sbc *c, uint8_t *out, size_t cap)
{
    if (cap < SBC_INFO_LEN)
        return 0;
    out[0] = (uint8_t)((c->freq & 0xF0) | (c->cm & 0x0F));
    out[1] = (uint8_t)((c->block & 0xF0) | (c->subbands & 0x0C) | (c->alloc & 0x03));
    out[2] = c->min_bitpool;
    out[3] = c->max_bitpool;
    return SBC_INFO_LEN;
}

/* First bit of `mask` in priority order. */
static uint8_t pick(uint8_t mask, const uint8_t *order, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (mask & order[i])
            return order[i];
    return 0;
}

static uint8_t ldac_channels(uint8_t cm)
{
    return cm == LDAC_CM_MONO ? 1 : 2;
}

int a2dp_select_ldac(const a2dp_ldac *local, const a2dp_ldac *peer, uint32_t pref_rate, a2dp_config *out)
{
    /* Highest rate first; 176.4/192 kHz are not supported by libldac. */
    static const uint8_t freq_order[] = { LDAC_FREQ_96000, LDAC_FREQ_48000, LDAC_FREQ_88200, LDAC_FREQ_44100 };
    static const uint8_t cm_order[] = { LDAC_CM_STEREO, LDAC_CM_DUAL, LDAC_CM_MONO };
    uint8_t freq = local->freq & peer->freq;
    uint8_t cm = local->cm & peer->cm;
    uint8_t pref = a2dp_ldac_rate_to_freq(pref_rate);

    if (!freq)
        return A2DP_ERR_INVALID_SAMPLING_FREQUENCY;
    if (!cm)
        return A2DP_ERR_INVALID_CHANNEL_MODE;

    memset(out, 0, sizeof(*out));
    out->codec = A2DP_SEL_LDAC;
    out->ldac.freq = (freq & pref) ? pref : pick(freq, freq_order, sizeof(freq_order));
    out->ldac.cm = pick(cm, cm_order, sizeof(cm_order));
    if (!out->ldac.freq || !out->ldac.cm)
        return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
    out->sample_rate = a2dp_ldac_freq_to_rate(out->ldac.freq);
    out->channels = ldac_channels(out->ldac.cm);
    return 0;
}

int a2dp_select_sbc(const a2dp_sbc *local, const a2dp_sbc *peer, uint32_t pref_rate, a2dp_config *out)
{
    static const uint8_t freq_order[] = { SBC_FREQ_48000, SBC_FREQ_44100, SBC_FREQ_32000, SBC_FREQ_16000 };
    static const uint8_t cm_order[] = { SBC_CM_JOINT, SBC_CM_STEREO, SBC_CM_DUAL, SBC_CM_MONO };
    static const uint8_t block_order[] = { SBC_BLOCK_16, SBC_BLOCK_12, SBC_BLOCK_8, SBC_BLOCK_4 };
    static const uint8_t sub_order[] = { SBC_SUBBANDS_8, SBC_SUBBANDS_4 };
    static const uint8_t alloc_order[] = { SBC_ALLOC_LOUDNESS, SBC_ALLOC_SNR };
    uint8_t freq = local->freq & peer->freq;
    uint8_t pref = 0;
    size_t i;
    uint8_t minbp, maxbp;

    for (i = 0; i < sizeof(sbc_rates) / sizeof(sbc_rates[0]); i++)
        if (sbc_rates[i].rate == pref_rate)
            pref = sbc_rates[i].bit;

    memset(out, 0, sizeof(*out));
    out->codec = A2DP_SEL_SBC;
    out->sbc.freq = (freq & pref) ? pref : pick(freq, freq_order, sizeof(freq_order));
    out->sbc.cm = pick(local->cm & peer->cm, cm_order, sizeof(cm_order));
    out->sbc.block = pick(local->block & peer->block, block_order, sizeof(block_order));
    out->sbc.subbands = pick(local->subbands & peer->subbands, sub_order, sizeof(sub_order));
    out->sbc.alloc = pick(local->alloc & peer->alloc, alloc_order, sizeof(alloc_order));
    if (!out->sbc.freq)
        return A2DP_ERR_INVALID_SAMPLING_FREQUENCY;
    if (!out->sbc.cm)
        return A2DP_ERR_INVALID_CHANNEL_MODE;
    if (!out->sbc.block || !out->sbc.subbands || !out->sbc.alloc)
        return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;

    minbp = local->min_bitpool > peer->min_bitpool ? local->min_bitpool : peer->min_bitpool;
    maxbp = local->max_bitpool < peer->max_bitpool ? local->max_bitpool : peer->max_bitpool;
    if (maxbp > SBC_BITPOOL_HQ)
        maxbp = SBC_BITPOOL_HQ;
    if (minbp < 2)
        minbp = 2;
    if (minbp > maxbp)
        return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
    out->sbc.min_bitpool = minbp;
    out->sbc.max_bitpool = maxbp;
    out->sample_rate = sbc_freq_to_rate(out->sbc.freq);
    out->channels = out->sbc.cm == SBC_CM_MONO ? 1 : 2;
    return 0;
}

int a2dp_check_ldac_config(const a2dp_ldac *local, const a2dp_ldac *cfg, a2dp_config *out)
{
    if (!ONE_BIT(cfg->freq) || !(cfg->freq & local->freq) || !a2dp_ldac_freq_to_rate(cfg->freq))
        return A2DP_ERR_INVALID_SAMPLING_FREQUENCY;
    if (!ONE_BIT(cfg->cm) || !(cfg->cm & local->cm))
        return A2DP_ERR_INVALID_CHANNEL_MODE;
    memset(out, 0, sizeof(*out));
    out->codec = A2DP_SEL_LDAC;
    out->ldac = *cfg;
    out->sample_rate = a2dp_ldac_freq_to_rate(cfg->freq);
    out->channels = ldac_channels(cfg->cm);
    return 0;
}

int a2dp_check_sbc_config(const a2dp_sbc *local, const a2dp_sbc *cfg, a2dp_config *out)
{
    if (!ONE_BIT(cfg->freq) || !(cfg->freq & local->freq))
        return A2DP_ERR_INVALID_SAMPLING_FREQUENCY;
    if (!ONE_BIT(cfg->cm) || !(cfg->cm & local->cm))
        return A2DP_ERR_INVALID_CHANNEL_MODE;
    if (!ONE_BIT(cfg->block) || !(cfg->block & local->block) ||
        !ONE_BIT(cfg->subbands) || !(cfg->subbands & local->subbands) ||
        !ONE_BIT(cfg->alloc) || !(cfg->alloc & local->alloc) ||
        cfg->min_bitpool < 2 || cfg->min_bitpool > cfg->max_bitpool || cfg->max_bitpool > 250)
        return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
    memset(out, 0, sizeof(*out));
    out->codec = A2DP_SEL_SBC;
    out->sbc = *cfg;
    out->sample_rate = sbc_freq_to_rate(cfg->freq);
    out->channels = cfg->cm == SBC_CM_MONO ? 1 : 2;
    return 0;
}
