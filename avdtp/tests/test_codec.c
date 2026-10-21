#include "a2dp_codec.h"
#include "avdtp.h"
#include "test.h"

static void test_ldac_ie(void)
{
    const uint8_t ie[] = { 0x2D, 0x01, 0x00, 0x00, 0xAA, 0x00, 0x3C, 0x07 };
    a2dp_ldac c;
    uint8_t out[8];
    size_t i;

    CHECK_EQ(a2dp_ldac_parse(A2DP_CODEC_VENDOR, ie, sizeof(ie), &c), 0);
    CHECK_EQ(c.freq, 0x3C);
    CHECK_EQ(c.cm, 0x07);
    CHECK_EQ(a2dp_ldac_build(&c, out, sizeof(out)), 8);
    for (i = 0; i < 8; i++)
        CHECK_EQ(out[i], ie[i]);
    CHECK_EQ(a2dp_ldac_parse(A2DP_CODEC_SBC, ie, sizeof(ie), &c), -1);
    CHECK_EQ(a2dp_ldac_parse(A2DP_CODEC_VENDOR, ie, 6, &c), -1);
    {
        uint8_t aptx[] = { 0x4F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x22 , 0x00 };
        CHECK_EQ(a2dp_ldac_parse(A2DP_CODEC_VENDOR, aptx, sizeof(aptx), &c), -1);
    }
}

static void test_ldac_select(void)
{
    a2dp_ldac local = { LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000,
                        LDAC_CM_STEREO | LDAC_CM_DUAL | LDAC_CM_MONO };
    a2dp_ldac peer = { LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000, LDAC_CM_STEREO };
    a2dp_ldac peer48 = { LDAC_FREQ_44100 | LDAC_FREQ_48000, LDAC_CM_STEREO | LDAC_CM_MONO };
    a2dp_ldac none = { LDAC_FREQ_192000, LDAC_CM_STEREO };
    a2dp_config cfg;

    CHECK_EQ(a2dp_select_ldac(&local, &peer, 96000, &cfg), 0);
    CHECK_EQ(cfg.sample_rate, 96000);
    CHECK_EQ(cfg.ldac.cm, LDAC_CM_STEREO);
    CHECK_EQ(cfg.channels, 2);
    CHECK_EQ(a2dp_select_ldac(&local, &peer, 44100, &cfg), 0);
    CHECK_EQ(cfg.sample_rate, 44100);
    CHECK_EQ(a2dp_select_ldac(&local, &peer48, 96000, &cfg), 0);
    CHECK_EQ(cfg.sample_rate, 48000);
    CHECK_EQ(a2dp_select_ldac(&local, &none, 96000, &cfg), A2DP_ERR_INVALID_SAMPLING_FREQUENCY);

    {
        a2dp_ldac one = { LDAC_FREQ_96000, LDAC_CM_STEREO };
        a2dp_ldac two = { LDAC_FREQ_96000 | LDAC_FREQ_48000, LDAC_CM_STEREO };
        CHECK_EQ(a2dp_check_ldac_config(&local, &one, &cfg), 0);
        CHECK_EQ(a2dp_check_ldac_config(&local, &two, &cfg), A2DP_ERR_INVALID_SAMPLING_FREQUENCY);
    }
}

static void test_sbc_select(void)
{
    a2dp_sbc local = { SBC_FREQ_44100 | SBC_FREQ_48000, 0x0F, 0xF0, 0x0C, 0x03, 2, SBC_BITPOOL_HQ };
    /* typical headset: everything, bitpool 2..53 */
    a2dp_sbc peer = { 0xF0, 0x0F, 0xF0, 0x0C, 0x03, 2, 53 };
    a2dp_sbc weak = { SBC_FREQ_44100, SBC_CM_MONO, SBC_BLOCK_16, SBC_SUBBANDS_8, SBC_ALLOC_LOUDNESS, 20, 35 };
    a2dp_config cfg;
    uint8_t ie[4];

    CHECK_EQ(a2dp_select_sbc(&local, &peer, 48000, &cfg), 0);
    CHECK_EQ(cfg.sample_rate, 48000);
    CHECK_EQ(cfg.sbc.cm, SBC_CM_JOINT);
    CHECK_EQ(cfg.sbc.block, SBC_BLOCK_16);
    CHECK_EQ(cfg.sbc.subbands, SBC_SUBBANDS_8);
    CHECK_EQ(cfg.sbc.alloc, SBC_ALLOC_LOUDNESS);
    CHECK_EQ(cfg.sbc.max_bitpool, 53);
    CHECK_EQ(a2dp_sbc_build(&cfg.sbc, ie, sizeof(ie)), 4);
    CHECK_EQ(ie[0], 0x11);
    CHECK_EQ(ie[1], 0x15);

    CHECK_EQ(a2dp_select_sbc(&local, &weak, 48000, &cfg), 0);
    CHECK_EQ(cfg.sample_rate, 44100);
    CHECK_EQ(cfg.channels, 1);
    CHECK_EQ(cfg.sbc.min_bitpool, 20);
    CHECK_EQ(cfg.sbc.max_bitpool, 35);
}

void test_codec(void)
{
    test_ldac_ie();
    test_ldac_select();
    test_sbc_select();
}
