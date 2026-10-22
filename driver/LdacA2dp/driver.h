/*
 * LdacA2dp: A2DP source profile driver for one Bluetooth sink.
 *
 * Installed as the function driver on the BTHENUM A2DP sink device
 * (service {0000110B-...}) in place of BthA2dp. Signaling runs through the
 * shared AVDTP core (avdtp/), channels are opened with BRBs submitted to
 * BthPort, media packets come from the user-mode encoder service.
 */
#pragma once

#include <ntddk.h>
#include <initguid.h>
#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <wdf.h>
#include <acx.h>
#include <ntstrsafe.h>
#include <bthdef.h>
#include <ntintsafe.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <sdpnode.h>
#include <bthddi.h>

#include "public.h"
#include "avdtp_source.h"

#define LDACA2DP_POOL_TAG 'adlA'
#define LDACA2DP_SIG_READ_SIZE 1024
#define LDACA2DP_SIG_MTU 1024
#define LDACA2DP_MEDIA_IN_MTU 895
#define LDACA2DP_MEDIA_FLUSH_TIMEOUT_MS 96 /* as Alternative A2DP does, docs/jbl-live-780nc.md */
#define LDACA2DP_SIGNAL_TIMEOUT_MS 5000    /* AVDTP RTX_SIG_TIMER upper bound */

typedef enum _CHANNEL_KIND { ChannelSignaling = 0, ChannelMedia = 1 } CHANNEL_KIND;

typedef struct _L2CAP_CHANNEL {
    struct _DEVICE_CONTEXT *Device;
    CHANNEL_KIND Kind;
    BOOLEAN Open;
    BOOLEAN Opening;
    BOOLEAN Incoming;          /* opened by the sink through our L2CAP server */
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT OutMtu;

    WDFREQUEST OpenRequest;    /* BRB_L2CA_OPEN_CHANNEL or, for Incoming, ..._RESPONSE */
    struct _BRB_L2CA_OPEN_CHANNEL OpenBrb;
    WDFREQUEST CloseRequest;
    struct _BRB_L2CA_CLOSE_CHANNEL CloseBrb;
} L2CAP_CHANNEL, *PL2CAP_CHANNEL;

/* Deferred work collected while holding the lock, executed after releasing it,
 * so BRB submissions never complete inline under our own spinlock. */
#define DEFER_OPEN_MEDIA   0x01
#define DEFER_CLOSE_MEDIA  0x02
#define DEFER_CLOSE_SIG    0x04
#define DEFER_ABORT        0x08

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;
    BTH_PROFILE_DRIVER_INTERFACE ProfileInterface;
    BTH_ADDR LocalAddress;
    BTH_ADDR RemoteAddress;
    L2CAP_SERVER_HANDLE ServerHandle; /* PSM 0x19 server filtered to RemoteAddress */
    BOOLEAN PsmRegistered;

    WDFSPINLOCK Lock;          /* protects everything below */
    avdtp_source Avdtp;
    a2dp_prefs Prefs;
    ULONG Deferred;
    LONG MediaInFlight;
    LONG64 MediaPacketsSent;
    LONG64 MediaPacketsFailed; /* rejected by the driver or failed in the controller */
    NTSTATUS LastError;
    WDFTIMER SignalTimer;

    L2CAP_CHANNEL Sig;
    L2CAP_CHANNEL Media;

    /* audio endpoint (audio.c): render circuit and the PCM handed to the service */
    ACXCIRCUIT Render;
    WDFSPINLOCK PcmLock;       /* protects the Pcm* fields */
    PUCHAR PcmRing;
    ULONG PcmRingSize;
    ULONG PcmReadPos;
    ULONG PcmCount;
    LDACA2DP_PCM_HEADER PcmFormat; /* Bytes unused; Flags carries a pending discontinuity */
    ULONG PcmBlockAlign;
    BOOLEAN PcmRunning;
    UINT64 PcmDroppedBytes;
    WDFQUEUE PcmReadQueue;     /* manual: READ_PCM waiting for audio */

    WDFREQUEST SigReadRequest;
    struct _BRB_L2CA_ACL_TRANSFER SigReadBrb;
    UCHAR SigReadBuffer[LDACA2DP_SIG_READ_SIZE];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* Every request carries room for one ACL transfer BRB, used by SEND_MEDIA. */
typedef struct _REQUEST_CONTEXT {
    struct _BRB_L2CA_ACL_TRANSFER Brb;
} REQUEST_CONTEXT, *PREQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT, GetRequestContext)

/* Signaling writes own a copy of the SDU. */
typedef struct _SIG_WRITE_CONTEXT {
    struct _BRB_L2CA_ACL_TRANSFER Brb;
    UCHAR Data[AVDTP_SIG_MTU_DEFAULT];
} SIG_WRITE_CONTEXT, *PSIG_WRITE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SIG_WRITE_CONTEXT, GetSigWriteContext)

/* Refusal of an incoming connection we have no channel for. */
typedef struct _REJECT_CONTEXT {
    struct _BRB_L2CA_OPEN_CHANNEL Brb;
} REJECT_CONTEXT, *PREJECT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REJECT_CONTEXT, GetRejectContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD LdacA2dpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE LdacA2dpEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT LdacA2dpEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP LdacA2dpEvtSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL LdacA2dpEvtIoDeviceControl;
EVT_WDF_TIMER LdacA2dpEvtSignalTimer;

/* l2cap.c */
NTSTATUS L2capInitialize(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS L2capQueryAddresses(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS L2capRegisterServer(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL) VOID L2capUnregisterServer(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS L2capOpenChannel(_In_ PL2CAP_CHANNEL Channel);
VOID L2capCloseChannel(_In_ PL2CAP_CHANNEL Channel);
_IRQL_requires_(PASSIVE_LEVEL) VOID L2capCloseChannelSync(_In_ PL2CAP_CHANNEL Channel);
NTSTATUS L2capSendSignal(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ size_t Length);
NTSTATUS L2capSendMedia(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_reads_bytes_(Length) PVOID Data,
                        _In_ size_t Length);
VOID L2capStartSignalReader(_In_ PDEVICE_CONTEXT Ctx);

/* audio.c */
#define LDACA2DP_PCM_RING_SIZE (256 * 1024) /* ~330 ms at 96 kHz / 32-bit stereo */
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AudioCreate(_In_ PDEVICE_CONTEXT Ctx);
VOID AudioReadPcm(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);
VOID AudioFillStatus(_In_ PDEVICE_CONTEXT Ctx, _Inout_ LDACA2DP_STATUS *Out);
EVT_WDF_DEVICE_RELEASE_HARDWARE LdacA2dpEvtReleaseHardware;

/* avdtp_glue.c */
extern const avdtp_src_ops LdacA2dpAvdtpOps;
VOID AvdtpRunDeferred(_In_ PDEVICE_CONTEXT Ctx);
VOID AvdtpOnSignalReceived(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ size_t Length);
VOID AvdtpOnChannelOpened(_In_ PL2CAP_CHANNEL Channel, _In_ NTSTATUS Status);
VOID AvdtpOnChannelClosed(_In_ PL2CAP_CHANNEL Channel);
