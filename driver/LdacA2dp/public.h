/*
 * Interface between the LdacA2dp kernel driver and the user-mode encoder service.
 * Shared by both sides: keep it plain C with fixed-size types.
 *
 * The driver configures the stream on its own when the device starts (default
 * preferences) and accepts AVDTP channels the sink opens itself.
 *
 * Flow: open the device interface -> IOCTL_LDACA2DP_CONNECT (preferences) ->
 * poll IOCTL_LDACA2DP_GET_STATUS until Streaming/Open -> IOCTL_LDACA2DP_START ->
 * one IOCTL_LDACA2DP_SEND_MEDIA per complete A2DP media packet (RTP header +
 * media payload header + codec frames). A SEND_MEDIA request completes when the
 * packet has been handed to the controller, so the number of requests the
 * service keeps in flight is its TX queue depth (input for LDAC ABR).
 */
#ifndef LDACA2DP_PUBLIC_H
#define LDACA2DP_PUBLIC_H

/* {365d7148-308e-4162-924f-06c40d52b82e} */
DEFINE_GUID(GUID_DEVINTERFACE_LDACA2DP,
    0x365d7148, 0x308e, 0x4162, 0x92, 0x4f, 0x06, 0xc4, 0x0d, 0x52, 0xb8, 0x2e);

#define LDACA2DP_IOCTL(fn, access) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (fn), METHOD_BUFFERED, access)

/* Status needs a handle opened for reading; anything that changes the link
 * or sends audio needs one opened for writing. */
#define IOCTL_LDACA2DP_CONNECT    LDACA2DP_IOCTL(0, FILE_WRITE_ACCESS) /* in: LDACA2DP_PREFS */
#define IOCTL_LDACA2DP_DISCONNECT LDACA2DP_IOCTL(1, FILE_WRITE_ACCESS)
#define IOCTL_LDACA2DP_START      LDACA2DP_IOCTL(2, FILE_WRITE_ACCESS)
#define IOCTL_LDACA2DP_SUSPEND    LDACA2DP_IOCTL(3, FILE_WRITE_ACCESS)
#define IOCTL_LDACA2DP_GET_STATUS LDACA2DP_IOCTL(4, FILE_READ_ACCESS)  /* out: LDACA2DP_STATUS */
#define IOCTL_LDACA2DP_SEND_MEDIA LDACA2DP_IOCTL(5, FILE_WRITE_ACCESS) /* in: one media packet */

#define LDACA2DP_MAX_INFLIGHT_MEDIA 32

/* Mirrors a2dp_prefs (avdtp/include/a2dp_codec.h); masks use the LDAC_FREQ_* / LDAC_CM_* bits. */
typedef struct _LDACA2DP_PREFS {
    UINT8 AllowLdac;
    UINT8 LdacFreqMask;
    UINT8 LdacChannelModeMask;
    UINT8 Reserved;
    UINT32 LdacPreferredRate;
    UINT32 SbcPreferredRate;
} LDACA2DP_PREFS;

typedef enum _LDACA2DP_CODEC {
    LdacA2dpCodecNone = 0,
    LdacA2dpCodecSbc = 1,
    LdacA2dpCodecLdac = 2,
} LDACA2DP_CODEC;

typedef struct _LDACA2DP_STATUS {
    UINT32 AvdtpState;        /* avdtp_src_state */
    UINT32 Codec;             /* LDACA2DP_CODEC */
    UINT32 SampleRate;
    UINT8 Channels;
    UINT8 LdacChannelMode;    /* LDAC_CM_* */
    UINT8 SbcConfig[4];       /* SBC codec information element when Codec == Sbc */
    UINT8 ServerRegistered;   /* L2CAP server for sink-initiated AVDTP is active */
    UINT8 SignalingIncoming;  /* the sink (not the driver) opened the signaling channel */
    UINT16 MediaMtu;          /* max media packet the peer accepts, 0 if channel closed */
    UINT16 DelayReport100us;  /* last AVDTP DELAYREPORT from the sink */
    UINT64 RemoteAddress;     /* BTH_ADDR of the headphones */
    UINT32 MediaInFlight;
    INT32 LastError;          /* NTSTATUS of the last failed channel operation */
    UINT64 MediaPacketsSent;
    UINT64 MediaPacketsFailed; /* rejected by the driver (state, size, queue full) or failed in the controller */
} LDACA2DP_STATUS;

#endif /* LDACA2DP_PUBLIC_H */
