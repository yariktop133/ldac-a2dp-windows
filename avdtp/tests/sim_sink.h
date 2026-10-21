/*
 * Simulated A2DP sink (headphones) for exercising avdtp_source without hardware.
 * Defaults mirror a real JBL Live 780NC (captured with btvs, see docs/jbl-live-780nc.md):
 * SEID 1 SBC, SEID 2 AAC, SEID 3 LDAC (44.1-96 kHz, all channel modes), delay reporting,
 * DELAYREPORT of 150 ms right after SET_CONFIGURATION.
 */
#ifndef SIM_SINK_H
#define SIM_SINK_H

#include "a2dp_codec.h"
#include "avdtp.h"

typedef struct sim_sink {
    /* configuration of the simulated device */
    uint8_t has_ldac;
    a2dp_ldac ldac;
    uint8_t no_get_all;      /* behave like AVDTP 1.2: GENERAL_REJECT GET_ALL_CAPABILITIES */
    uint8_t reject_ldac;     /* reject SET_CONFIGURATION for the LDAC SEP */
    uint8_t initiate;        /* sink drives DISCOVER/SET_CONFIGURATION/OPEN itself */
    uint8_t delay_report;    /* send DELAYREPORT after being configured */
    uint16_t delay_100us;

    /* observed state */
    uint8_t configured_seid;
    uint8_t remote_seid;
    a2dp_config config;
    uint8_t opened;
    uint8_t streaming;

    /* output queue towards the source */
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    void *ctx;
    avdtp_reasm reasm;
    uint8_t label;
    uint8_t init_step;
} sim_sink;

#define SIM_SEID_SBC 1
#define SIM_SEID_AAC 2
#define SIM_SEID_LDAC 3

void sim_sink_init(sim_sink *k, int (*send)(void *, const uint8_t *, size_t), void *ctx);
void sim_sink_connect(sim_sink *k); /* used when k->initiate */
void sim_sink_on_signal(sim_sink *k, const uint8_t *pkt, size_t len);

#endif
