/*
 * Binds the transport-agnostic AVDTP source (avdtp/src/avdtp_source.c) to
 * the L2CAP channels. Every call into the core happens under Ctx->Lock; the
 * core's callbacks only record work in Ctx->Deferred, which AvdtpRunDeferred
 * performs after the lock is dropped.
 */
#include "driver.h"

static int OpsSend(void *context, const uint8_t *buf, size_t len)
{
    return NT_SUCCESS(L2capSendSignal((PDEVICE_CONTEXT)context, buf, len)) ? 0 : -1;
}

static void OpsMediaOpen(void *context, int initiate)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)context;

    /* initiate == 0: the sink sent OPEN and connects the media channel itself;
     * the L2CAP server accepts it (AcceptIncoming). The signal timer aborts the
     * stream if it never arrives. */
    if (initiate)
        ctx->Deferred |= DEFER_OPEN_MEDIA;
}

static void OpsMediaClose(void *context)
{
    ((PDEVICE_CONTEXT)context)->Deferred |= DEFER_CLOSE_MEDIA;
}

const avdtp_src_ops LdacA2dpAvdtpOps = { OpsSend, OpsMediaOpen, OpsMediaClose, NULL, NULL, NULL };

VOID AvdtpRunDeferred(_In_ PDEVICE_CONTEXT Ctx)
{
    for (;;) {
        ULONG work;
        BOOLEAN pending;

        WdfSpinLockAcquire(Ctx->Lock);
        work = Ctx->Deferred;
        Ctx->Deferred = 0;
        if (work & DEFER_ABORT)
            avdtp_src_abort(&Ctx->Avdtp);
        /* Also bound the wait for the media channel, whoever opens it. */
        pending = Ctx->Avdtp.pend_active != 0 || Ctx->Avdtp.state == AVDTP_SRC_WAIT_MEDIA;
        WdfSpinLockRelease(Ctx->Lock);

        if (pending)
            WdfTimerStart(Ctx->SignalTimer, WDF_REL_TIMEOUT_IN_MS(LDACA2DP_SIGNAL_TIMEOUT_MS));
        else
            WdfTimerStop(Ctx->SignalTimer, FALSE);

        if (work & DEFER_CLOSE_MEDIA)
            L2capCloseChannel(&Ctx->Media);
        if (work & DEFER_CLOSE_SIG)
            L2capCloseChannel(&Ctx->Sig);
        if (work & DEFER_OPEN_MEDIA) {
            NTSTATUS status = L2capOpenChannel(&Ctx->Media);
            if (!NT_SUCCESS(status)) {
                WdfSpinLockAcquire(Ctx->Lock);
                Ctx->LastError = status;
                Ctx->Deferred |= DEFER_ABORT;
                WdfSpinLockRelease(Ctx->Lock);
            }
        }

        WdfSpinLockAcquire(Ctx->Lock);
        work = Ctx->Deferred;
        WdfSpinLockRelease(Ctx->Lock);
        if (!work)
            return;
    }
}

VOID AvdtpOnSignalReceived(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ size_t Length)
{
    WdfSpinLockAcquire(Ctx->Lock);
    (VOID)avdtp_src_on_signal(&Ctx->Avdtp, Data, Length);
    WdfSpinLockRelease(Ctx->Lock);
    AvdtpRunDeferred(Ctx);
}

VOID AvdtpOnChannelOpened(_In_ PL2CAP_CHANNEL Channel, _In_ NTSTATUS Status)
{
    PDEVICE_CONTEXT ctx = Channel->Device;

    WdfSpinLockAcquire(ctx->Lock);
    if (!NT_SUCCESS(Status)) {
        ctx->LastError = Status;
        if (Channel->Kind == ChannelMedia)
            ctx->Deferred |= DEFER_ABORT;
    } else if (Channel->Kind == ChannelSignaling) {
        /* We opened signaling: act as initiator (DISCOVER ... OPEN). If the sink
         * opened it, it initiates and the core answers as acceptor. */
        if (!Channel->Incoming)
            (VOID)avdtp_src_connect(&ctx->Avdtp);
    } else if (ctx->Avdtp.state == AVDTP_SRC_WAIT_MEDIA) {
        avdtp_src_media_connected(&ctx->Avdtp);
    } else {
        /* The stream was aborted while the channel was being set up. */
        ctx->Deferred |= DEFER_CLOSE_MEDIA;
    }
    WdfSpinLockRelease(ctx->Lock);
    AvdtpRunDeferred(ctx);
}

VOID AvdtpOnChannelClosed(_In_ PL2CAP_CHANNEL Channel)
{
    PDEVICE_CONTEXT ctx = Channel->Device;

    WdfSpinLockAcquire(ctx->Lock);
    Channel->Open = FALSE;
    if (Channel->Kind == ChannelSignaling) {
        /* Without signaling the stream is gone; release both channel handles. */
        avdtp_src_disconnected(&ctx->Avdtp);
        ctx->Deferred |= DEFER_CLOSE_SIG | DEFER_CLOSE_MEDIA;
    } else {
        if (ctx->Avdtp.state >= AVDTP_SRC_WAIT_MEDIA && ctx->Avdtp.state <= AVDTP_SRC_SUSPENDING)
            ctx->Deferred |= DEFER_ABORT;
        ctx->Deferred |= DEFER_CLOSE_MEDIA;
    }
    WdfSpinLockRelease(ctx->Lock);
    AvdtpRunDeferred(ctx);
}
