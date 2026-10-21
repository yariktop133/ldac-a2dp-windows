/*
 * L2CAP channels to the sink through BthPort BRBs (bthddi.h), following the
 * pattern of the WDK bthecho sample. BRBs are passed in Parameters.Others.Argument1
 * of IOCTL_INTERNAL_BTH_SUBMIT_BRB.
 */
#include "driver.h"

static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE CloseCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE SignalReadCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE SignalWriteCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE MediaWriteCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE RejectCompletion;

static VOID FormatBrb(_In_ WDFREQUEST Request, _In_ PVOID Brb)
{
    IO_STACK_LOCATION stack;

    RtlZeroMemory(&stack, sizeof(stack));
    stack.MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack.Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_BTH_SUBMIT_BRB;
    stack.Parameters.Others.Argument1 = Brb;
    WdfRequestWdmFormatUsingStackLocation(Request, &stack);
}

/* Sends a driver-created, reusable request carrying `Brb`. */
static NTSTATUS SendBrbAsync(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ PVOID Brb,
                             _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion, _In_opt_ WDFCONTEXT Context)
{
    WDF_REQUEST_REUSE_PARAMS reuse;

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(Request, &reuse);
    FormatBrb(Request, Brb);
    WdfRequestSetCompletionRoutine(Request, Completion, Context);
    if (!WdfRequestSend(Request, Ctx->IoTarget, WDF_NO_SEND_OPTIONS))
        return WdfRequestGetStatus(Request);
    return STATUS_PENDING;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SendBrbSync(_In_ PDEVICE_CONTEXT Ctx, _In_ PVOID Brb)
{
    WDFREQUEST request;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->Device;
    status = WdfRequestCreate(&attributes, Ctx->IoTarget, &request);
    if (!NT_SUCCESS(status))
        return status;
    FormatBrb(request, Brb);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    (VOID)WdfRequestSend(request, Ctx->IoTarget, &options);
    status = WdfRequestGetStatus(request);
    WdfObjectDelete(request);
    return status;
}

static NTSTATUS CreateRequest(_In_ PDEVICE_CONTEXT Ctx, _Out_ WDFREQUEST *Request)
{
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->Device;
    return WdfRequestCreate(&attributes, Ctx->IoTarget, Request);
}

NTSTATUS L2capInitialize(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    if (Ctx->SigReadRequest)
        return STATUS_SUCCESS; /* PrepareHardware after a restart */
    status = CreateRequest(Ctx, &Ctx->Sig.OpenRequest);
    if (NT_SUCCESS(status))
        status = CreateRequest(Ctx, &Ctx->Sig.CloseRequest);
    if (NT_SUCCESS(status))
        status = CreateRequest(Ctx, &Ctx->Media.OpenRequest);
    if (NT_SUCCESS(status))
        status = CreateRequest(Ctx, &Ctx->Media.CloseRequest);
    if (NT_SUCCESS(status))
        status = CreateRequest(Ctx, &Ctx->SigReadRequest);
    return status;
}

NTSTATUS L2capQueryAddresses(_In_ PDEVICE_CONTEXT Ctx)
{
    struct _BRB_GET_LOCAL_BD_ADDR localBrb;
    BTH_DEVICE_INFO info;
    WDF_MEMORY_DESCRIPTOR out;
    NTSTATUS status;

    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));
    status = WdfIoTargetSendInternalIoctlSynchronously(Ctx->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO,
                                                       NULL, &out, NULL, NULL);
    if (!NT_SUCCESS(status))
        return status;
    Ctx->RemoteAddress = info.address;

    RtlZeroMemory(&localBrb, sizeof(localBrb));
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&localBrb, BRB_HCI_GET_LOCAL_BD_ADDR);
    status = SendBrbSync(Ctx, &localBrb);
    if (NT_SUCCESS(status))
        Ctx->LocalAddress = localBrb.BtAddress;
    return status;
}

static VOID L2capIndication(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Parameters)
{
    PL2CAP_CHANNEL channel = (PL2CAP_CHANNEL)Context;

    UNREFERENCED_PARAMETER(Parameters);
    if (Indication == IndicationRemoteDisconnect)
        AvdtpOnChannelClosed(channel);
}

/* Channel parameters shared by outgoing opens and responses to incoming ones. */
static VOID FillChannelConfig(_In_ PL2CAP_CHANNEL Channel, _Inout_ struct _BRB_L2CA_OPEN_CHANNEL *brb)
{
    PDEVICE_CONTEXT ctx = Channel->Device;

    brb->BtAddress = ctx->RemoteAddress;
    brb->ChannelFlags = CF_ROLE_EITHER;

    /* Outbound: accept whatever receive MTU the sink offers (JBL: 679). */
    brb->ConfigOut.Flags = CFG_MTU;
    brb->ConfigOut.Mtu.Max = L2CAP_MAX_MTU;
    brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigOut.Mtu.Preferred = L2CAP_MAX_MTU;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu.Max = Channel->Kind == ChannelSignaling ? LDACA2DP_SIG_MTU : LDACA2DP_MEDIA_IN_MTU;
    brb->ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigIn.Mtu.Preferred = brb->ConfigIn.Mtu.Max;
    if (Channel->Kind == ChannelMedia) {
        /* Stale audio is useless: let the controller drop it after 96 ms. */
        brb->ConfigOut.Flags |= CFG_FLUSHTO;
        brb->ConfigOut.FlushTO.Max = LDACA2DP_MEDIA_FLUSH_TIMEOUT_MS;
        brb->ConfigOut.FlushTO.Min = LDACA2DP_MEDIA_FLUSH_TIMEOUT_MS;
        brb->ConfigOut.FlushTO.Preferred = LDACA2DP_MEDIA_FLUSH_TIMEOUT_MS;
    }
    brb->IncomingQueueDepth = 16;
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = L2capIndication;
    brb->CallbackContext = Channel;
    brb->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(ctx->Device);
}

NTSTATUS L2capOpenChannel(_In_ PL2CAP_CHANNEL Channel)
{
    PDEVICE_CONTEXT ctx = Channel->Device;
    struct _BRB_L2CA_OPEN_CHANNEL *brb = &Channel->OpenBrb;
    NTSTATUS status;

    WdfSpinLockAcquire(ctx->Lock);
    if (Channel->Open || Channel->Opening || Channel->Handle) {
        WdfSpinLockRelease(ctx->Lock);
        return STATUS_DEVICE_BUSY;
    }
    Channel->Opening = TRUE;
    Channel->Incoming = FALSE;
    WdfSpinLockRelease(ctx->Lock);

    ctx->ProfileInterface.BthInitializeBrb((PBRB)brb, BRB_L2CA_OPEN_CHANNEL);
    FillChannelConfig(Channel, brb);
    brb->Psm = AVDTP_PSM;

    status = SendBrbAsync(ctx, Channel->OpenRequest, brb, OpenCompletion, Channel);
    if (status != STATUS_PENDING) {
        WdfSpinLockAcquire(ctx->Lock);
        Channel->Opening = FALSE;
        WdfSpinLockRelease(ctx->Lock);
    }
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

static VOID OpenCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                           _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PL2CAP_CHANNEL channel = (PL2CAP_CHANNEL)Context;
    PDEVICE_CONTEXT ctx = channel->Device;
    NTSTATUS status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(ctx->Lock);
    channel->Opening = FALSE;
    if (NT_SUCCESS(status)) {
        channel->Handle = channel->OpenBrb.ChannelHandle;
        channel->OutMtu = channel->OpenBrb.OutResults.Params.Mtu;
        channel->Open = TRUE;
    }
    WdfSpinLockRelease(ctx->Lock);

    if (NT_SUCCESS(status) && channel->Kind == ChannelSignaling)
        L2capStartSignalReader(ctx);
    AvdtpOnChannelOpened(channel, status);
}

/* ---- L2CAP server: sinks often open AVDTP themselves after reconnecting ---- */

static VOID RejectIncoming(_In_ PDEVICE_CONTEXT Ctx, _In_ PINDICATION_PARAMETERS Params)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PREJECT_CONTEXT reject;
    WDFREQUEST request;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REJECT_CONTEXT);
    attributes.ParentObject = Ctx->Device;
    if (!NT_SUCCESS(WdfRequestCreate(&attributes, Ctx->IoTarget, &request)))
        return;
    reject = GetRejectContext(request);
    RtlZeroMemory(&reject->Brb, sizeof(reject->Brb));
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&reject->Brb, BRB_L2CA_OPEN_CHANNEL_RESPONSE);
    reject->Brb.BtAddress = Params->BtAddress;
    reject->Brb.ChannelHandle = Params->ConnectionHandle;
    reject->Brb.Response = CONNECT_RSP_RESULT_NO_RESOURCES;

    FormatBrb(request, &reject->Brb);
    WdfRequestSetCompletionRoutine(request, RejectCompletion, NULL);
    if (!WdfRequestSend(request, Ctx->IoTarget, WDF_NO_SEND_OPTIONS))
        WdfObjectDelete(request);
}

static VOID RejectCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                             _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);
    WdfObjectDelete(Request);
}

static VOID AcceptIncoming(_In_ PDEVICE_CONTEXT Ctx, _In_ PINDICATION_PARAMETERS Params)
{
    PL2CAP_CHANNEL channel = NULL;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    NTSTATUS status;

    /* AVDTP: the first transport channel of a session is signaling, the next
     * one is media. Claim the channel under the lock so that two connections
     * arriving back to back cannot both land on signaling. */
    WdfSpinLockAcquire(Ctx->Lock);
    if (!Ctx->Sig.Open && !Ctx->Sig.Opening && !Ctx->Sig.Handle)
        channel = &Ctx->Sig;
    else if (Ctx->Sig.Open && !Ctx->Media.Open && !Ctx->Media.Opening && !Ctx->Media.Handle)
        channel = &Ctx->Media;
    if (channel) {
        channel->Opening = TRUE;
        channel->Incoming = TRUE;
    }
    WdfSpinLockRelease(Ctx->Lock);

    if (!channel) {
        RejectIncoming(Ctx, Params);
        return;
    }

    brb = &channel->OpenBrb;
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)brb, BRB_L2CA_OPEN_CHANNEL_RESPONSE);
    FillChannelConfig(channel, brb);
    brb->ChannelHandle = Params->ConnectionHandle;
    brb->Response = CONNECT_RSP_RESULT_SUCCESS;

    status = SendBrbAsync(Ctx, channel->OpenRequest, brb, OpenCompletion, channel);
    if (status != STATUS_PENDING) {
        WdfSpinLockAcquire(Ctx->Lock);
        channel->Opening = FALSE;
        WdfSpinLockRelease(Ctx->Lock);
    }
}

static VOID ServerIndication(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Parameters)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    /* The server is registered for RemoteAddress only; the check is a safety net
     * so we never answer for another device that BthA2dp should handle. */
    if (Indication == IndicationRemoteConnect && Parameters->BtAddress == ctx->RemoteAddress)
        AcceptIncoming(ctx, Parameters);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS L2capRegisterServer(_In_ PDEVICE_CONTEXT Ctx)
{
    struct _BRB_PSM psm;
    struct _BRB_L2CA_REGISTER_SERVER reg;
    NTSTATUS status;

    /* PSM 0x19 is a fixed PSM that other profile drivers (BthA2dp for other
     * sinks) may already hold; a failure here is expected and harmless, the
     * address-filtered server registration below is what matters. */
    RtlZeroMemory(&psm, sizeof(psm));
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&psm, BRB_REGISTER_PSM);
    psm.Psm = AVDTP_PSM;
    Ctx->PsmRegistered = NT_SUCCESS(SendBrbSync(Ctx, &psm));

    RtlZeroMemory(&reg, sizeof(reg));
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&reg, BRB_L2CA_REGISTER_SERVER);
    reg.BtAddress = Ctx->RemoteAddress;
    reg.PSM = AVDTP_PSM;
    reg.IndicationFlags = 0;
    reg.IndicationCallback = ServerIndication;
    reg.IndicationCallbackContext = Ctx;
    reg.ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(Ctx->Device);
    status = SendBrbSync(Ctx, &reg);
    if (NT_SUCCESS(status))
        Ctx->ServerHandle = reg.ServerHandle;
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID L2capUnregisterServer(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->ServerHandle) {
        struct _BRB_L2CA_UNREGISTER_SERVER brb;
        RtlZeroMemory(&brb, sizeof(brb));
        Ctx->ProfileInterface.BthInitializeBrb((PBRB)&brb, BRB_L2CA_UNREGISTER_SERVER);
        brb.BtAddress = Ctx->RemoteAddress;
        brb.Psm = AVDTP_PSM;
        brb.ServerHandle = Ctx->ServerHandle;
        (VOID)SendBrbSync(Ctx, &brb);
        Ctx->ServerHandle = NULL;
    }
    if (Ctx->PsmRegistered) {
        struct _BRB_PSM brb;
        RtlZeroMemory(&brb, sizeof(brb));
        Ctx->ProfileInterface.BthInitializeBrb((PBRB)&brb, BRB_UNREGISTER_PSM);
        brb.Psm = AVDTP_PSM;
        (VOID)SendBrbSync(Ctx, &brb);
        Ctx->PsmRegistered = FALSE;
    }
}

VOID L2capCloseChannel(_In_ PL2CAP_CHANNEL Channel)
{
    PDEVICE_CONTEXT ctx = Channel->Device;
    struct _BRB_L2CA_CLOSE_CHANNEL *brb = &Channel->CloseBrb;
    L2CAP_CHANNEL_HANDLE handle;

    WdfSpinLockAcquire(ctx->Lock);
    handle = Channel->Handle;
    Channel->Handle = NULL;
    Channel->Open = FALSE;
    Channel->OutMtu = 0;
    WdfSpinLockRelease(ctx->Lock);
    if (!handle)
        return;

    ctx->ProfileInterface.BthInitializeBrb((PBRB)brb, BRB_L2CA_CLOSE_CHANNEL);
    brb->BtAddress = ctx->RemoteAddress;
    brb->ChannelHandle = handle;
    (VOID)SendBrbAsync(ctx, Channel->CloseRequest, brb, CloseCompletion, Channel);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID L2capCloseChannelSync(_In_ PL2CAP_CHANNEL Channel)
{
    PDEVICE_CONTEXT ctx = Channel->Device;
    struct _BRB_L2CA_CLOSE_CHANNEL brb;
    L2CAP_CHANNEL_HANDLE handle;

    WdfSpinLockAcquire(ctx->Lock);
    handle = Channel->Handle;
    Channel->Handle = NULL;
    Channel->Open = FALSE;
    Channel->OutMtu = 0;
    WdfSpinLockRelease(ctx->Lock);
    if (!handle)
        return;

    RtlZeroMemory(&brb, sizeof(brb));
    ctx->ProfileInterface.BthInitializeBrb((PBRB)&brb, BRB_L2CA_CLOSE_CHANNEL);
    brb.BtAddress = ctx->RemoteAddress;
    brb.ChannelHandle = handle;
    (VOID)SendBrbSync(ctx, &brb);
}

static VOID CloseCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                            _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);
}

/* ---- signaling: one continuously re-armed read, writes own their buffer -- */

VOID L2capStartSignalReader(_In_ PDEVICE_CONTEXT Ctx)
{
    struct _BRB_L2CA_ACL_TRANSFER *brb = &Ctx->SigReadBrb;
    L2CAP_CHANNEL_HANDLE handle;

    WdfSpinLockAcquire(Ctx->Lock);
    handle = Ctx->Sig.Open ? Ctx->Sig.Handle : NULL;
    WdfSpinLockRelease(Ctx->Lock);
    if (!handle)
        return;

    Ctx->ProfileInterface.BthInitializeBrb((PBRB)brb, BRB_L2CA_ACL_TRANSFER);
    brb->BtAddress = Ctx->RemoteAddress;
    brb->ChannelHandle = handle;
    brb->TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
    brb->Buffer = Ctx->SigReadBuffer;
    brb->BufferSize = sizeof(Ctx->SigReadBuffer);
    brb->BufferMDL = NULL;
    (VOID)SendBrbAsync(Ctx, Ctx->SigReadRequest, brb, SignalReadCompletion, Ctx);
}

static VOID SignalReadCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                                 _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    if (!NT_SUCCESS(Params->IoStatus.Status))
        return; /* channel closed or request cancelled: the reader stops */
    if (ctx->SigReadBrb.BufferSize <= sizeof(ctx->SigReadBuffer))
        AvdtpOnSignalReceived(ctx, ctx->SigReadBuffer, ctx->SigReadBrb.BufferSize);
    L2capStartSignalReader(ctx);
}

NTSTATUS L2capSendSignal(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ size_t Length)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PSIG_WRITE_CONTEXT write;
    WDFREQUEST request;
    NTSTATUS status;

    /* Called from the AVDTP core under Ctx->Lock: Sig fields are stable here. */
    if (!Ctx->Sig.Open || Length > sizeof(write->Data))
        return STATUS_INVALID_DEVICE_STATE;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SIG_WRITE_CONTEXT);
    attributes.ParentObject = Ctx->Device;
    status = WdfRequestCreate(&attributes, Ctx->IoTarget, &request);
    if (!NT_SUCCESS(status))
        return status;

    write = GetSigWriteContext(request);
    RtlCopyMemory(write->Data, Data, Length);
    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&write->Brb, BRB_L2CA_ACL_TRANSFER);
    write->Brb.BtAddress = Ctx->RemoteAddress;
    write->Brb.ChannelHandle = Ctx->Sig.Handle;
    write->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    write->Brb.Buffer = write->Data;
    write->Brb.BufferSize = (ULONG)Length;
    write->Brb.BufferMDL = NULL;

    FormatBrb(request, &write->Brb);
    WdfRequestSetCompletionRoutine(request, SignalWriteCompletion, NULL);
    if (!WdfRequestSend(request, Ctx->IoTarget, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(request);
        WdfObjectDelete(request);
        return status;
    }
    return STATUS_SUCCESS;
}

static VOID SignalWriteCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                                  _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);
    WdfObjectDelete(Request);
}

/* ---- media: the service's IOCTL request goes down as the ACL write ------ */

NTSTATUS L2capSendMedia(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_reads_bytes_(Length) PVOID Data,
                        _In_ size_t Length)
{
    PREQUEST_CONTEXT rc = GetRequestContext(Request);
    L2CAP_CHANNEL_HANDLE handle;

    WdfSpinLockAcquire(Ctx->Lock);
    handle = Ctx->Media.Open ? Ctx->Media.Handle : NULL;
    WdfSpinLockRelease(Ctx->Lock);
    if (!handle)
        return STATUS_INVALID_DEVICE_STATE;

    Ctx->ProfileInterface.BthInitializeBrb((PBRB)&rc->Brb, BRB_L2CA_ACL_TRANSFER);
    rc->Brb.BtAddress = Ctx->RemoteAddress;
    rc->Brb.ChannelHandle = handle;
    rc->Brb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    rc->Brb.Buffer = Data;
    rc->Brb.BufferSize = (ULONG)Length;
    rc->Brb.BufferMDL = NULL;

    FormatBrb(Request, &rc->Brb);
    WdfRequestSetCompletionRoutine(Request, MediaWriteCompletion, Ctx);
    InterlockedIncrement(&Ctx->MediaInFlight);
    if (!WdfRequestSend(Request, Ctx->IoTarget, WDF_NO_SEND_OPTIONS)) {
        InterlockedDecrement(&Ctx->MediaInFlight);
        return WdfRequestGetStatus(Request);
    }
    return STATUS_PENDING;
}

static VOID MediaWriteCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                                 _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Target);
    InterlockedDecrement(&ctx->MediaInFlight);
    InterlockedIncrement64(NT_SUCCESS(Params->IoStatus.Status) ? &ctx->MediaPacketsSent : &ctx->MediaPacketsFailed);
    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status,
                                      NT_SUCCESS(Params->IoStatus.Status) ? GetRequestContext(Request)->Brb.BufferSize : 0);
}
