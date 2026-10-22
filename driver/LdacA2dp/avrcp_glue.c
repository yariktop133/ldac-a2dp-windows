/*
 * AVRCP role: binds the AVRCP core (avdtp/src/avrcp.c) to the AVCTP channel
 * (PSM 0x17, carried in the device's "Sig" channel slot), reports the
 * headphones' buttons as a VHF consumer-control HID device and links the
 * absolute volume to the A2DP device of the same headphones (peer table).
 *
 * Locking: the core runs under Ctx->Lock and only records keys and volume
 * reports; they leave (VHF, peer) after the lock is dropped. The peer table
 * has its own spin lock and is always taken before a device lock, never
 * while holding one.
 */
#include "driver.h"

/* ---- peers: the A2DP and AVRCP devices of one pair of headphones ------------ */

#define MAX_PEERS 8

typedef struct _PEER {
    BTH_ADDR Address;
    PDEVICE_CONTEXT A2dp;
    PDEVICE_CONTEXT Avrcp;
} PEER;

static KSPIN_LOCK g_PeerLock;
static PEER g_Peers[MAX_PEERS];

VOID PeerInitialize(VOID)
{
    KeInitializeSpinLock(&g_PeerLock);
    RtlZeroMemory(g_Peers, sizeof(g_Peers));
}

static PEER *FindPeer(_In_ BTH_ADDR Address, _In_ BOOLEAN Create)
{
    ULONG i;
    PEER *free = NULL;

    for (i = 0; i < MAX_PEERS; i++) {
        if (g_Peers[i].Address == Address && (g_Peers[i].A2dp || g_Peers[i].Avrcp))
            return &g_Peers[i];
        if (!free && !g_Peers[i].A2dp && !g_Peers[i].Avrcp)
            free = &g_Peers[i];
    }
    if (Create && free)
        free->Address = Address;
    return Create ? free : NULL;
}

VOID PeerAttach(_In_ PDEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    PEER *p;
    PDEVICE_CONTEXT avrcpToConnect = NULL;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(Ctx->RemoteAddress, TRUE);
    if (p) {
        if (Ctx->Role == RoleA2dp)
            p->A2dp = Ctx;
        else
            p->Avrcp = Ctx;
        /* AVRCP coming up while the stream is already open: connect AVCTP */
        if (p->A2dp && p->Avrcp && p->A2dp->Media.Open)
            avrcpToConnect = p->Avrcp;
        if (avrcpToConnect) {
            InterlockedExchange(&avrcpToConnect->ConnectTries, 3);
            avrcpToConnect->RetryPending = TRUE;
            WdfTimerStart(avrcpToConnect->SignalTimer, WDF_REL_TIMEOUT_IN_MS(500));
        }
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

VOID PeerDetach(_In_ PDEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    PEER *p;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(Ctx->RemoteAddress, FALSE);
    if (p) {
        if (p->A2dp == Ctx)
            p->A2dp = NULL;
        if (p->Avrcp == Ctx)
            p->Avrcp = NULL;
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

VOID PeerSendVolume(_In_ PDEVICE_CONTEXT A2dp, _In_ UCHAR Volume)
{
    KIRQL irql;
    PEER *p;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(A2dp->RemoteAddress, FALSE);
    if (p && p->Avrcp) {
        WdfSpinLockAcquire(p->Avrcp->Lock);
        (VOID)avrcp_set_volume(&p->Avrcp->Avrcp, Volume);
        WdfSpinLockRelease(p->Avrcp->Lock);
        WdfTimerStart(p->Avrcp->SignalTimer, WDF_REL_TIMEOUT_IN_MS(2000));
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

VOID PeerSetPlaying(_In_ PDEVICE_CONTEXT A2dp, _In_ BOOLEAN Playing)
{
    KIRQL irql;
    PEER *p;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(A2dp->RemoteAddress, FALSE);
    if (p && p->Avrcp) {
        WdfSpinLockAcquire(p->Avrcp->Lock);
        avrcp_set_playing(&p->Avrcp->Avrcp, Playing);
        WdfSpinLockRelease(p->Avrcp->Lock);
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

VOID PeerA2dpConnected(_In_ PDEVICE_CONTEXT A2dp)
{
    KIRQL irql;
    PEER *p;

    /* AVRCP follows A2DP (the headphones may also open AVCTP themselves; the
     * open then fails with "busy" and their channel is accepted instead). */
    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(A2dp->RemoteAddress, FALSE);
    if (p && p->Avrcp) {
        InterlockedExchange(&p->Avrcp->ConnectTries, 3);
        /* give the headphones a moment to open it themselves first */
        p->Avrcp->RetryPending = TRUE;
        WdfTimerStart(p->Avrcp->SignalTimer, WDF_REL_TIMEOUT_IN_MS(1500));
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

BOOLEAN PeerAbsoluteVolume(_In_ PDEVICE_CONTEXT A2dp)
{
    KIRQL irql;
    PEER *p;
    BOOLEAN yes = FALSE;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(A2dp->RemoteAddress, FALSE);
    if (p && p->Avrcp)
        yes = p->Avrcp->Avrcp.connected && p->Avrcp->Avrcp.abs_volume == AVRCP_ABSVOL_YES;
    KeReleaseSpinLock(&g_PeerLock, irql);
    return yes;
}

UCHAR PeerAvrcpState(_In_ PDEVICE_CONTEXT A2dp, _Out_ PLONG LastError)
{
    KIRQL irql;
    PEER *p;
    UCHAR state = LDACA2DP_AVRCP_NO_DEVICE;

    *LastError = 0;
    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(A2dp->RemoteAddress, FALSE);
    if (p && p->Avrcp) {
        PDEVICE_CONTEXT a = p->Avrcp;
        *LastError = a->LastError;
        state = a->Sig.Opening ? LDACA2DP_AVRCP_OPENING
              : !a->Sig.Open ? LDACA2DP_AVRCP_CLOSED
              : a->Avrcp.abs_volume == AVRCP_ABSVOL_YES ? LDACA2DP_AVRCP_ABS_VOLUME
              : a->Avrcp.abs_volume == AVRCP_ABSVOL_NO ? LDACA2DP_AVRCP_NO_ABS
                                                       : LDACA2DP_AVRCP_CONNECTED;
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
    return state;
}

/* The headphones changed their volume: hand it to the A2DP device's endpoint. */
static VOID PeerVolumeFromHeadphones(_In_ PDEVICE_CONTEXT Avrcp, _In_ UCHAR Volume)
{
    KIRQL irql;
    PEER *p;

    KeAcquireSpinLock(&g_PeerLock, &irql);
    p = FindPeer(Avrcp->RemoteAddress, FALSE);
    if (p && p->A2dp && p->A2dp->VolumeWorkItem) {
        InterlockedExchange(&p->A2dp->RemoteVolume, Volume);
        WdfWorkItemEnqueue(p->A2dp->VolumeWorkItem);
    }
    KeReleaseSpinLock(&g_PeerLock, irql);
}

/* ---- the AVRCP core's callbacks (under Ctx->Lock) --------------------------- */

static int OpsSend(void *context, const uint8_t *buf, size_t len)
{
    return NT_SUCCESS(L2capSendSignal((PDEVICE_CONTEXT)context, buf, len)) ? 0 : -1;
}

static void OpsVolume(void *context, uint8_t volume)
{
    ((PDEVICE_CONTEXT)context)->HeadphoneVolume = volume;
}

static USHORT KeyUsage(_In_ uint8_t Op)
{
    switch (Op) {
    case AVRCP_OP_PLAY:
    case AVRCP_OP_PAUSE:
        return 0xCD; /* Play/Pause: Windows' media session decides which */
    case AVRCP_OP_STOP: return 0xB7;
    case AVRCP_OP_FORWARD: return 0xB5;
    case AVRCP_OP_BACKWARD: return 0xB6;
    case AVRCP_OP_FAST_FORWARD: return 0xB3;
    case AVRCP_OP_REWIND: return 0xB4;
    case AVRCP_OP_VOLUME_UP: return 0xE9;
    case AVRCP_OP_VOLUME_DOWN: return 0xEA;
    case AVRCP_OP_MUTE: return 0xE2;
    default: return 0;
    }
}

static void OpsKey(void *context, uint8_t op, int pressed)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)context;
    USHORT usage = KeyUsage(op);

    if (usage && ctx->KeyCount < RTL_NUMBER_OF(ctx->Keys))
        ctx->Keys[ctx->KeyCount++] = pressed ? (UCHAR)usage : 0;
}

static const avrcp_ops LdacA2dpAvrcpOps = { OpsSend, OpsVolume, OpsKey };

/* ---- buttons: VHF consumer control ------------------------------------------- */

/* One 16-bit consumer usage per report, 0 = nothing pressed. */
static UCHAR g_ConsumerReport[] = {
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03, /*   Logical Maximum (1023) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03, /*   Usage Maximum (1023) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x00,       /*   Input (Data, Array, Absolute) */
    0xC0,             /* End Collection */
};

static VOID SendKey(_In_ PDEVICE_CONTEXT Ctx, _In_ USHORT Usage)
{
    UCHAR report[2] = { (UCHAR)Usage, (UCHAR)(Usage >> 8) };
    HID_XFER_PACKET packet;

    if (!Ctx->Vhf)
        return;
    packet.reportBuffer = report;
    packet.reportBufferLen = sizeof(report);
    packet.reportId = 0;
    (VOID)VhfReadReportSubmit(Ctx->Vhf, &packet); /* VHF copies the report */
}

/* Work the core recorded under the lock: keys, the headphones' volume. */
static VOID AvrcpFlush(_In_ PDEVICE_CONTEXT Ctx)
{
    UCHAR keys[RTL_NUMBER_OF(Ctx->Keys)];
    ULONG count, i;
    LONG volume;
    BOOLEAN waiting;

    WdfSpinLockAcquire(Ctx->Lock);
    count = Ctx->KeyCount;
    RtlCopyMemory(keys, Ctx->Keys, count);
    Ctx->KeyCount = 0;
    volume = Ctx->HeadphoneVolume;
    Ctx->HeadphoneVolume = -1;
    waiting = Ctx->Avrcp.caps_pending || Ctx->Avrcp.set_pending;
    WdfSpinLockRelease(Ctx->Lock);

    for (i = 0; i < count; i++)
        SendKey(Ctx, keys[i]);
    if (volume >= 0)
        PeerVolumeFromHeadphones(Ctx, (UCHAR)volume);
    if (waiting)
        WdfTimerStart(Ctx->SignalTimer, WDF_REL_TIMEOUT_IN_MS(2000));
}

/* ---- channel events from l2cap.c ---------------------------------------------- */

VOID AvrcpOnSignalReceived(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ size_t Length)
{
    WdfSpinLockAcquire(Ctx->Lock);
    (VOID)avrcp_input(&Ctx->Avrcp, Data, Length);
    WdfSpinLockRelease(Ctx->Lock);
    AvrcpFlush(Ctx);
}

VOID AvrcpOnChannelOpened(_In_ PL2CAP_CHANNEL Channel, _In_ NTSTATUS Status)
{
    PDEVICE_CONTEXT ctx = Channel->Device;

    if (!NT_SUCCESS(Status)) {
        ctx->LastError = Status; /* the headphones may still open AVCTP themselves */
        if (InterlockedCompareExchange(&ctx->ConnectTries, 0, 0) > 0) {
            ctx->RetryPending = TRUE;
            WdfTimerStart(ctx->SignalTimer, WDF_REL_TIMEOUT_IN_MS(1500));
        }
        return;
    }
    InterlockedExchange(&ctx->ConnectTries, 0);
    WdfSpinLockAcquire(ctx->Lock);
    avrcp_connected(&ctx->Avrcp);
    WdfSpinLockRelease(ctx->Lock);
    AvrcpFlush(ctx);
}

VOID AvrcpOnChannelClosed(_In_ PL2CAP_CHANNEL Channel)
{
    PDEVICE_CONTEXT ctx = Channel->Device;

    WdfSpinLockAcquire(ctx->Lock);
    Channel->Open = FALSE;
    avrcp_disconnected(&ctx->Avrcp);
    WdfSpinLockRelease(ctx->Lock);
    L2capCloseChannel(Channel); /* release the handle */
}

VOID AvrcpOnTimer(_In_ PDEVICE_CONTEXT Ctx)
{
    BOOLEAN connect = FALSE;

    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->RetryPending) {
        Ctx->RetryPending = FALSE;
        connect = !Ctx->Sig.Open && !Ctx->Sig.Opening && InterlockedDecrement(&Ctx->ConnectTries) >= 0;
    }
    WdfSpinLockRelease(Ctx->Lock);
    if (connect)
        (VOID)L2capOpenChannel(&Ctx->Sig);

    WdfSpinLockAcquire(Ctx->Lock);
    avrcp_timeout(&Ctx->Avrcp);
    WdfSpinLockRelease(Ctx->Lock);
    AvrcpFlush(Ctx);
}

/* ---- start / stop (self-managed I/O) ------------------------------------------ */

NTSTATUS AvrcpStart(_In_ PDEVICE_CONTEXT Ctx)
{
    VHF_CONFIG vhf;
    NTSTATUS status;

    PAGED_CODE();
    WdfSpinLockAcquire(Ctx->Lock);
    avrcp_init(&Ctx->Avrcp, &LdacA2dpAvrcpOps, Ctx);
    Ctx->HeadphoneVolume = -1;
    Ctx->KeyCount = 0;
    WdfSpinLockRelease(Ctx->Lock);

    /* Buttons: a consumer-control HID device below which vhf.sys sits as lower
     * filter (INF). Without it AVRCP still runs, just without buttons. */
    VHF_CONFIG_INIT(&vhf, WdfDeviceWdmGetDeviceObject(Ctx->Device), sizeof(g_ConsumerReport), g_ConsumerReport);
    vhf.VhfClientContext = Ctx;
    status = VhfCreate(&vhf, &Ctx->Vhf);
    if (NT_SUCCESS(status)) {
        status = VhfStart(Ctx->Vhf);
        if (!NT_SUCCESS(status)) {
            VhfDelete(Ctx->Vhf, TRUE);
            Ctx->Vhf = NULL;
        }
    } else {
        Ctx->Vhf = NULL;
    }

    status = L2capRegisterServer(Ctx); /* the headphones often open AVCTP themselves */
    PeerAttach(Ctx);                   /* connects AVCTP if A2DP is already up */
    return status;
}

VOID AvrcpStop(_In_ PDEVICE_CONTEXT Ctx)
{
    PAGED_CODE();
    PeerDetach(Ctx);
    if (Ctx->Vhf) {
        VhfDelete(Ctx->Vhf, TRUE);
        Ctx->Vhf = NULL;
    }
}
