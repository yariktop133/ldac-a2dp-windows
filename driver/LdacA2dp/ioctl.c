#include "driver.h"

static VOID PrefsFromUser(_In_ const LDACA2DP_PREFS *in, _Out_ a2dp_prefs *out)
{
    a2dp_prefs_default(out);
    out->allow_ldac = in->AllowLdac != 0;
    if (in->LdacFreqMask)
        out->ldac_freq_mask = in->LdacFreqMask;
    if (in->LdacChannelModeMask)
        out->ldac_cm_mask = in->LdacChannelModeMask;
    if (in->LdacPreferredRate)
        out->ldac_pref_rate = in->LdacPreferredRate;
    if (in->SbcPreferredRate)
        out->sbc_pref_rate = in->SbcPreferredRate;
}

static NTSTATUS Connect(_In_ PDEVICE_CONTEXT Ctx, _In_ const LDACA2DP_PREFS *Prefs)
{
    BOOLEAN sigOpen;
    NTSTATUS status = STATUS_SUCCESS;

    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->Avdtp.state != AVDTP_SRC_IDLE && Ctx->Avdtp.state != AVDTP_SRC_FAILED) {
        WdfSpinLockRelease(Ctx->Lock);
        return STATUS_DEVICE_BUSY;
    }
    PrefsFromUser(Prefs, &Ctx->Prefs);
    avdtp_src_init(&Ctx->Avdtp, &LdacA2dpAvdtpOps, Ctx, &Ctx->Prefs);
    Ctx->Avdtp.auto_start = 0;
    Ctx->LastError = STATUS_SUCCESS;
    sigOpen = Ctx->Sig.Open;
    if (sigOpen && avdtp_src_connect(&Ctx->Avdtp) != 0)
        status = STATUS_INVALID_DEVICE_STATE;
    WdfSpinLockRelease(Ctx->Lock);

    if (!sigOpen) {
        status = L2capOpenChannel(&Ctx->Sig); /* DISCOVER follows in AvdtpOnChannelOpened */
        if (status == STATUS_DEVICE_BUSY)
            status = STATUS_SUCCESS; /* auto-connect already opening it; the new prefs apply */
    }
    AvdtpRunDeferred(Ctx);
    return status;
}

static VOID FillStatus(_In_ PDEVICE_CONTEXT Ctx, _Out_ LDACA2DP_STATUS *Out)
{
    const a2dp_config *cfg = &Ctx->Avdtp.config;

    RtlZeroMemory(Out, sizeof(*Out));
    WdfSpinLockAcquire(Ctx->Lock);
    Out->AvdtpState = (UINT32)Ctx->Avdtp.state;
    Out->Codec = cfg->codec == A2DP_SEL_LDAC ? LdacA2dpCodecLdac
               : cfg->codec == A2DP_SEL_SBC  ? LdacA2dpCodecSbc
                                             : LdacA2dpCodecNone;
    Out->SampleRate = cfg->sample_rate;
    Out->Channels = cfg->channels;
    Out->LdacChannelMode = cfg->ldac.cm;
    if (cfg->codec == A2DP_SEL_SBC)
        (VOID)a2dp_sbc_build(&cfg->sbc, Out->SbcConfig, sizeof(Out->SbcConfig));
    Out->MediaMtu = Ctx->Media.Open ? Ctx->Media.OutMtu : 0;
    Out->DelayReport100us = Ctx->Avdtp.delay_100us;
    Out->RemoteAddress = Ctx->RemoteAddress;
    Out->LastError = Ctx->LastError;
    Out->ServerRegistered = Ctx->ServerHandle != NULL;
    Out->SignalingIncoming = Ctx->Sig.Open && Ctx->Sig.Incoming;
    WdfSpinLockRelease(Ctx->Lock);
    Out->MediaPacketsSent = (UINT64)InterlockedCompareExchange64(&Ctx->MediaPacketsSent, 0, 0);
    Out->MediaPacketsFailed = (UINT64)InterlockedCompareExchange64(&Ctx->MediaPacketsFailed, 0, 0);
    Out->MediaInFlight = (UINT32)InterlockedCompareExchange(&Ctx->MediaInFlight, 0, 0);
}

VOID LdacA2dpEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                                _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    PVOID buffer;
    size_t length;
    int rc;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_LDACA2DP_CONNECT:
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(LDACA2DP_PREFS), &buffer, NULL);
        if (NT_SUCCESS(status))
            status = Connect(ctx, (const LDACA2DP_PREFS *)buffer);
        break;

    case IOCTL_LDACA2DP_DISCONNECT:
        WdfSpinLockAcquire(ctx->Lock);
        avdtp_src_disconnected(&ctx->Avdtp);
        ctx->Deferred |= DEFER_CLOSE_MEDIA | DEFER_CLOSE_SIG;
        WdfSpinLockRelease(ctx->Lock);
        AvdtpRunDeferred(ctx);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_LDACA2DP_START:
    case IOCTL_LDACA2DP_SUSPEND:
        WdfSpinLockAcquire(ctx->Lock);
        rc = IoControlCode == IOCTL_LDACA2DP_START ? avdtp_src_start(&ctx->Avdtp) : avdtp_src_suspend(&ctx->Avdtp);
        WdfSpinLockRelease(ctx->Lock);
        AvdtpRunDeferred(ctx);
        status = rc == 0 ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
        break;

    case IOCTL_LDACA2DP_GET_STATUS:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(LDACA2DP_STATUS), &buffer, NULL);
        if (NT_SUCCESS(status)) {
            FillStatus(ctx, (LDACA2DP_STATUS *)buffer);
            info = sizeof(LDACA2DP_STATUS);
        }
        break;

    case IOCTL_LDACA2DP_SEND_MEDIA:
        status = WdfRequestRetrieveInputBuffer(Request, 13, &buffer, &length);
        if (!NT_SUCCESS(status))
            break;
        WdfSpinLockAcquire(ctx->Lock);
        if (ctx->Avdtp.state != AVDTP_SRC_STREAMING || !ctx->Media.Open)
            status = STATUS_INVALID_DEVICE_STATE;
        else if (length > ctx->Media.OutMtu)
            status = STATUS_BUFFER_OVERFLOW;
        WdfSpinLockRelease(ctx->Lock);
        if (NT_SUCCESS(status) && InterlockedCompareExchange(&ctx->MediaInFlight, 0, 0) >= LDACA2DP_MAX_INFLIGHT_MEDIA)
            status = STATUS_DEVICE_BUSY;
        if (NT_SUCCESS(status)) {
            status = L2capSendMedia(ctx, Request, buffer, length);
            if (status == STATUS_PENDING)
                return; /* completed and counted in MediaWriteCompletion */
        }
        InterlockedIncrement64(&ctx->MediaPacketsFailed);
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}
