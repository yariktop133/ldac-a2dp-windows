/*
 * Windows audio endpoint of the headphones (stage 5, option C).
 *
 * An ACX render circuit on the BTHENUM device itself, like Alternative A2DP
 * does with PortCls: host pin (KSCATEGORY_AUDIO) -> bridge pin (headphones).
 * There is no hardware behind the WaveRT buffer: a timer consumes it at the
 * stream rate (the scheme of the WDK ACX AudioCodec sample) and each finished
 * packet is copied into a ring the encoder service drains with READ_PCM.
 * Encoding stays in user mode, see docs/stage5-audio-options.md.
 */
#include "driver.h"

/* {5c9b3c1e-6b07-4d55-9a3a-1d0c43f0a2b1} */
DEFINE_GUID(LDACA2DP_RENDER_COMPONENT_GUID, 0x5c9b3c1e, 0x6b07, 0x4d55, 0x9a, 0x3a, 0x1d, 0x0c, 0x43, 0xf0, 0xa2, 0xb1);
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, which ksmedia.h only declares together with mmreg.h */
DEFINE_GUID(LDACA2DP_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DECLARE_CONST_UNICODE_STRING(LdacA2dpRenderCircuitName, L"Headphones0"); /* = INF interface reference string */

#define HNS_PER_SEC 10000000ULL
#define MAX_RT_PACKETS 2

typedef enum _RENDER_PIN { RenderHostPin = 0, RenderBridgePin = 1, RenderPinCount = 2 } RENDER_PIN;

typedef struct _CIRCUIT_CONTEXT {
    PDEVICE_CONTEXT Dev;
} CIRCUIT_CONTEXT, *PCIRCUIT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CIRCUIT_CONTEXT, GetCircuitContext)

typedef struct _RENDER_STREAM {
    PDEVICE_CONTEXT Dev;
    ACXSTREAM Stream;
    ACXDATAFORMAT Format;
    ULONG BytesPerSec;
    ULONG BlockAlign;
    PVOID Packets[MAX_RT_PACKETS];
    ULONG PacketCount;
    ULONG PacketSize;
    ULONG FirstPacketOffset;
    WDFTIMER Timer;
    ACX_STREAM_STATE State;
    volatile LONG CurrentPacket;
    ULONGLONG StartTime;    /* 100 ns, QPC based */
    ULONGLONG StartPosition;
    ULONGLONG Position;     /* bytes */
    ULONGLONG GlitchAdjust; /* 100 ns the timer ran late */
    LARGE_INTEGER QpcFrequency;
} RENDER_STREAM, *PRENDER_STREAM;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RENDER_STREAM, GetRenderStream)

typedef struct _STREAM_TIMER_CONTEXT {
    PRENDER_STREAM Stream;
} STREAM_TIMER_CONTEXT, *PSTREAM_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(STREAM_TIMER_CONTEXT, GetStreamTimerContext)

static EVT_ACX_CIRCUIT_CREATE_STREAM AudioEvtCreateStream;
static EVT_ACX_STREAM_PREPARE_HARDWARE AudioEvtStreamPrepareHardware;
static EVT_ACX_STREAM_RELEASE_HARDWARE AudioEvtStreamReleaseHardware;
static EVT_ACX_STREAM_RUN AudioEvtStreamRun;
static EVT_ACX_STREAM_PAUSE AudioEvtStreamPause;
static EVT_ACX_STREAM_ALLOCATE_RTPACKETS AudioEvtAllocateRtPackets;
static EVT_ACX_STREAM_FREE_RTPACKETS AudioEvtFreeRtPackets;
static EVT_ACX_STREAM_GET_HW_LATENCY AudioEvtGetHwLatency;
static EVT_ACX_STREAM_SET_RENDER_PACKET AudioEvtSetRenderPacket;
static EVT_ACX_STREAM_GET_CURRENT_PACKET AudioEvtGetCurrentPacket;
static EVT_ACX_STREAM_GET_PRESENTATION_POSITION AudioEvtGetPresentationPosition;
static EVT_WDF_TIMER AudioEvtPacketTimer;
static EVT_WDF_OBJECT_CONTEXT_DESTROY AudioEvtStreamDestroy;

/* ---- PCM ring (any IRQL <= DISPATCH) ---------------------------------------- */

static VOID PcmSetFormat(_In_ PDEVICE_CONTEXT Ctx, _In_ ACXDATAFORMAT Format, _In_ BOOLEAN Running)
{
    PWAVEFORMATEXTENSIBLE wfx = (PWAVEFORMATEXTENSIBLE)AcxDataFormatGetWaveFormatExtensible(Format);
    LDACA2DP_PCM_HEADER f;

    RtlZeroMemory(&f, sizeof(f));
    f.SampleRate = AcxDataFormatGetSampleRate(Format);
    f.Channels = (UINT16)AcxDataFormatGetChannelsCount(Format);
    f.BitsPerSample = (UINT16)AcxDataFormatGetBitsPerSample(Format);
    f.ValidBits = (UINT16)AcxDataFormatGetValidBitsPerSample(Format);
    if (wfx && IsEqualGUID(&wfx->SubFormat, &LDACA2DP_SUBTYPE_IEEE_FLOAT))
        f.Flags |= LDACA2DP_PCM_FLOAT;

    WdfSpinLockAcquire(Ctx->PcmLock);
    if (Ctx->PcmFormat.SampleRate != f.SampleRate || Ctx->PcmFormat.Channels != f.Channels ||
        Ctx->PcmFormat.BitsPerSample != f.BitsPerSample || (Ctx->PcmFormat.Flags & LDACA2DP_PCM_FLOAT) != f.Flags) {
        Ctx->PcmReadPos = 0;
        Ctx->PcmCount = 0; /* old-format audio is useless now */
        Ctx->PcmFormat = f;
        Ctx->PcmFormat.Flags |= LDACA2DP_PCM_DISCONTINUITY;
        Ctx->PcmBlockAlign = AcxDataFormatGetBlockAlign(Format);
    }
    Ctx->PcmRunning = Running;
    WdfSpinLockRelease(Ctx->PcmLock);
}

static VOID PcmSetRunning(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Running)
{
    WdfSpinLockAcquire(Ctx->PcmLock);
    Ctx->PcmRunning = Running;
    WdfSpinLockRelease(Ctx->PcmLock);
}

/* Fill one waiting READ_PCM from the ring. Called with PcmLock held. */
static NTSTATUS PcmFill(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _Out_ size_t *Info)
{
    LDACA2DP_PCM_HEADER *hdr;
    size_t outLen;
    ULONG n, first, block = Ctx->PcmBlockAlign ? Ctx->PcmBlockAlign : 1;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*hdr) + block, (PVOID *)&hdr, &outLen);

    *Info = 0;
    if (!NT_SUCCESS(status))
        return status;
    n = (ULONG)min((size_t)Ctx->PcmCount, outLen - sizeof(*hdr));
    n -= n % block;
    *hdr = Ctx->PcmFormat;
    hdr->Bytes = n;
    Ctx->PcmFormat.Flags &= ~LDACA2DP_PCM_DISCONTINUITY;

    first = min(n, Ctx->PcmRingSize - Ctx->PcmReadPos);
    RtlCopyMemory(hdr + 1, Ctx->PcmRing + Ctx->PcmReadPos, first);
    RtlCopyMemory((PUCHAR)(hdr + 1) + first, Ctx->PcmRing, n - first);
    Ctx->PcmReadPos = (Ctx->PcmReadPos + n) % Ctx->PcmRingSize;
    Ctx->PcmCount -= n;
    *Info = sizeof(*hdr) + n;
    return STATUS_SUCCESS;
}

/* Hand waiting READ_PCM requests whatever the ring holds. */
static VOID PcmDeliver(_In_ PDEVICE_CONTEXT Ctx)
{
    for (;;) {
        WDFREQUEST request;
        NTSTATUS status;
        size_t info;

        WdfSpinLockAcquire(Ctx->PcmLock);
        if (Ctx->PcmCount < Ctx->PcmBlockAlign || !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->PcmReadQueue, &request))) {
            WdfSpinLockRelease(Ctx->PcmLock);
            return;
        }
        status = PcmFill(Ctx, request, &info);
        WdfSpinLockRelease(Ctx->PcmLock);
        WdfRequestCompleteWithInformation(request, status, info);
    }
}

static VOID PcmWrite(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    ULONG size = Ctx->PcmRingSize, writePos, first;

    if (Length > size) {
        Data += Length - size;
        Length = size;
    }
    WdfSpinLockAcquire(Ctx->PcmLock);
    if (Ctx->PcmCount + Length > size) { /* nobody reads: drop the oldest */
        ULONG drop = Ctx->PcmCount + Length - size;
        ULONG block = Ctx->PcmBlockAlign ? Ctx->PcmBlockAlign : 1;
        drop += (block - drop % block) % block;
        drop = min(drop, Ctx->PcmCount);
        Ctx->PcmReadPos = (Ctx->PcmReadPos + drop) % size;
        Ctx->PcmCount -= drop;
        Ctx->PcmDroppedBytes += drop;
        Ctx->PcmFormat.Flags |= LDACA2DP_PCM_DISCONTINUITY;
    }
    writePos = (Ctx->PcmReadPos + Ctx->PcmCount) % size;
    first = min(Length, size - writePos);
    RtlCopyMemory(Ctx->PcmRing + writePos, Data, first);
    RtlCopyMemory(Ctx->PcmRing, Data + first, Length - first);
    Ctx->PcmCount += Length;
    WdfSpinLockRelease(Ctx->PcmLock);
    PcmDeliver(Ctx);
}

VOID AudioReadPcm(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request)
{
    NTSTATUS status;
    size_t info = 0;

    WdfSpinLockAcquire(Ctx->PcmLock);
    if (Ctx->PcmCount >= Ctx->PcmBlockAlign && Ctx->PcmBlockAlign) {
        status = PcmFill(Ctx, Request, &info);
        WdfSpinLockRelease(Ctx->PcmLock);
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }
    WdfSpinLockRelease(Ctx->PcmLock);
    /* Nothing yet: wait in the manual queue (cancellable there). Audio written
     * in between is picked up by the next packet's PcmDeliver, 10 ms later. */
    status = WdfRequestForwardToIoQueue(Request, Ctx->PcmReadQueue);
    if (!NT_SUCCESS(status))
        WdfRequestComplete(Request, status);
}

VOID AudioFillStatus(_In_ PDEVICE_CONTEXT Ctx, _Inout_ LDACA2DP_STATUS *Out)
{
    WdfSpinLockAcquire(Ctx->PcmLock);
    Out->PcmSampleRate = Ctx->PcmFormat.SampleRate;
    Out->PcmChannels = Ctx->PcmFormat.Channels;
    Out->PcmBitsPerSample = Ctx->PcmFormat.BitsPerSample;
    Out->PcmRunning = Ctx->PcmRunning;
    Out->PcmBuffered = Ctx->PcmCount;
    Out->PcmDroppedBytes = Ctx->PcmDroppedBytes;
    WdfSpinLockRelease(Ctx->PcmLock);
}

/* ---- circuit ------------------------------------------------------------------ */

static NTSTATUS AddFormat(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXDATAFORMATLIST List,
                          _In_ ULONG Rate, _In_ USHORT Bits, _In_ USHORT ValidBits, _In_ BOOLEAN Default)
{
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE f;
    ACX_DATAFORMAT_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACXDATAFORMAT format;
    NTSTATUS status;

    RtlZeroMemory(&f, sizeof(f));
    f.DataFormat.FormatSize = sizeof(f);
    f.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    f.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    f.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    f.WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.WaveFormatExt.Format.nChannels = 2;
    f.WaveFormatExt.Format.nSamplesPerSec = Rate;
    f.WaveFormatExt.Format.nBlockAlign = (WORD)(2 * Bits / 8);
    f.WaveFormatExt.Format.nAvgBytesPerSec = Rate * f.WaveFormatExt.Format.nBlockAlign;
    f.WaveFormatExt.Format.wBitsPerSample = Bits;
    f.WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.WaveFormatExt.Samples.wValidBitsPerSample = ValidBits;
    f.WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    f.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    ACX_DATAFORMAT_CONFIG_INIT_KS(&cfg, &f);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Circuit;
    status = AcxDataFormatCreate(Device, &attributes, &cfg, &format);
    if (!NT_SUCCESS(status))
        return status;
    status = AcxDataFormatListAddDataFormat(List, format);
    if (NT_SUCCESS(status) && Default)
        status = AcxDataFormatListAssignDefaultDataFormat(List, format);
    return status;
}

NTSTATUS AudioCreate(_In_ PDEVICE_CONTEXT Ctx)
{
    WDFDEVICE device = Ctx->Device;
    PACXCIRCUIT_INIT init;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACX_PIN_CONFIG pinCfg;
    ACXPIN pins[RenderPinCount];
    ACXDATAFORMATLIST formats;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFMEMORY ringMemory;
    ACXCIRCUIT circuit;
    NTSTATUS status;

    PAGED_CODE();

    /* PCM ring and the queue for READ_PCM waiting on it */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, LDACA2DP_POOL_TAG, LDACA2DP_PCM_RING_SIZE, &ringMemory,
                             (PVOID *)&Ctx->PcmRing);
    if (!NT_SUCCESS(status))
        return status;
    Ctx->PcmRingSize = LDACA2DP_PCM_RING_SIZE;
    status = WdfSpinLockCreate(&attributes, &Ctx->PcmLock);
    if (!NT_SUCCESS(status))
        return status;
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->PcmReadQueue);
    if (!NT_SUCCESS(status))
        return status;

    init = AcxCircuitInitAllocate(device);
    if (!init)
        return STATUS_INSUFFICIENT_RESOURCES;
    AcxCircuitInitSetComponentId(init, &LDACA2DP_RENDER_COMPONENT_GUID);
    status = AcxCircuitInitAssignName(init, &LdacA2dpRenderCircuitName);
    if (NT_SUCCESS(status)) {
        AcxCircuitInitSetCircuitType(init, AcxCircuitTypeRender);
        status = AcxCircuitInitAssignAcxCreateStreamCallback(init, AudioEvtCreateStream);
    }
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CIRCUIT_CONTEXT);
    status = AcxCircuitCreate(device, &attributes, &init, &circuit); /* frees init either way */
    if (!NT_SUCCESS(status))
        return status;
    GetCircuitContext(circuit)->Dev = Ctx;

    /* host pin: where the audio engine writes; no volume/mute elements, so
     * Windows applies software volume (AVRCP absolute volume comes later) */
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSink;
    pinCfg.Communication = AcxPinCommunicationSink;
    pinCfg.Category = &KSCATEGORY_AUDIO;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinCfg, &pins[RenderHostPin]);
    if (!NT_SUCCESS(status))
        return status;

    /* bridge pin: the headphones (endpoint form factor) */
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSource;
    pinCfg.Communication = AcxPinCommunicationNone;
    pinCfg.Category = &KSNODETYPE_HEADPHONES;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pinCfg, &pins[RenderBridgePin]);
    if (!NT_SUCCESS(status))
        return status;

    /* the encoder resamples to the link rate, so offer the usual mix formats;
     * 96 kHz / 24-bit matches LDAC at its best */
    formats = AcxPinGetRawDataFormatList(pins[RenderHostPin]);
    if (!formats)
        return STATUS_INSUFFICIENT_RESOURCES;
    status = AddFormat(device, circuit, formats, 44100, 16, 16, FALSE);
    if (NT_SUCCESS(status))
        status = AddFormat(device, circuit, formats, 48000, 16, 16, TRUE);
    if (NT_SUCCESS(status))
        status = AddFormat(device, circuit, formats, 48000, 32, 24, FALSE);
    if (NT_SUCCESS(status))
        status = AddFormat(device, circuit, formats, 88200, 32, 24, FALSE);
    if (NT_SUCCESS(status))
        status = AddFormat(device, circuit, formats, 96000, 32, 24, FALSE);
    if (!NT_SUCCESS(status))
        return status;

    status = AcxCircuitAddPins(circuit, pins, RenderPinCount);
    if (NT_SUCCESS(status))
        Ctx->Render = circuit;
    return status;
}

/* ---- stream --------------------------------------------------------------------- */

static NTSTATUS AudioEvtCreateStream(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXPIN Pin,
                                     _In_ PACXSTREAM_INIT StreamInit, _In_ ACXDATAFORMAT StreamFormat,
                                     _In_ const GUID *SignalProcessingMode, _In_ ACXOBJECTBAG VarArguments)
{
    ACX_STREAM_CALLBACKS streamCallbacks;
    ACX_RT_STREAM_CALLBACKS rtCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    ACXSTREAM stream;
    PRENDER_STREAM rs;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);
    PAGED_CODE();

    ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
    streamCallbacks.EvtAcxStreamPrepareHardware = AudioEvtStreamPrepareHardware;
    streamCallbacks.EvtAcxStreamReleaseHardware = AudioEvtStreamReleaseHardware;
    streamCallbacks.EvtAcxStreamRun = AudioEvtStreamRun;
    streamCallbacks.EvtAcxStreamPause = AudioEvtStreamPause;
    status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &streamCallbacks);
    if (!NT_SUCCESS(status))
        return status;

    ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
    rtCallbacks.EvtAcxStreamGetHwLatency = AudioEvtGetHwLatency;
    rtCallbacks.EvtAcxStreamAllocateRtPackets = AudioEvtAllocateRtPackets;
    rtCallbacks.EvtAcxStreamFreeRtPackets = AudioEvtFreeRtPackets;
    rtCallbacks.EvtAcxStreamSetRenderPacket = AudioEvtSetRenderPacket;
    rtCallbacks.EvtAcxStreamGetCurrentPacket = AudioEvtGetCurrentPacket;
    rtCallbacks.EvtAcxStreamGetPresentationPosition = AudioEvtGetPresentationPosition;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rtCallbacks);
    if (!NT_SUCCESS(status))
        return status;
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RENDER_STREAM);
    attributes.EvtDestroyCallback = AudioEvtStreamDestroy;
    status = AcxRtStreamCreate(Device, Circuit, &attributes, &StreamInit, &stream);
    if (!NT_SUCCESS(status))
        return status;

    rs = GetRenderStream(stream);
    RtlZeroMemory(rs, sizeof(*rs));
    rs->Dev = GetCircuitContext(Circuit)->Dev;
    rs->Stream = stream;
    rs->Format = StreamFormat;
    rs->BytesPerSec = AcxDataFormatGetAverageBytesPerSec(StreamFormat);
    rs->BlockAlign = AcxDataFormatGetBlockAlign(StreamFormat);
    rs->State = AcxStreamStateStop;
    KeQueryPerformanceCounter(&rs->QpcFrequency);
    return STATUS_SUCCESS;
}

static VOID AudioEvtStreamDestroy(_In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object); /* packets are freed by EvtAcxStreamFreeRtPackets */
}

static NTSTATUS AudioEvtAllocateRtPackets(_In_ ACXSTREAM Stream, _In_ ULONG PacketCount, _In_ ULONG PacketSize,
                                          _Out_ PACX_RTPACKET *Packets)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);
    PACX_RTPACKET packets;
    ULONG i, allocSize;

    PAGED_CODE();
    *Packets = NULL;
    if (PacketCount == 0 || PacketCount > MAX_RT_PACKETS || PacketSize == 0 || PacketSize > 16 * 1024 * 1024)
        return STATUS_INVALID_PARAMETER;
    packets = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, PacketCount * sizeof(ACX_RTPACKET), LDACA2DP_POOL_TAG);
    if (!packets)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* each packet on its own pages; the first one ends on a page boundary */
    allocSize = ROUND_TO_PAGES(PacketSize);
    for (i = 0; i < PacketCount; i++) {
        PVOID buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocSize, LDACA2DP_POOL_TAG);
        PMDL mdl = buffer ? IoAllocateMdl(buffer, allocSize, FALSE, FALSE, NULL) : NULL;

        ACX_RTPACKET_INIT(&packets[i]);
        if (!mdl) {
            if (buffer)
                ExFreePoolWithTag(buffer, LDACA2DP_POOL_TAG);
            AudioEvtFreeRtPackets(Stream, packets, i);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mdl);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[i].RtPacketBuffer, mdl, allocSize);
        packets[i].RtPacketSize = PacketSize;
        packets[i].RtPacketOffset = i == 0 ? allocSize - PacketSize : 0;
        rs->Packets[i] = buffer;
    }
    rs->PacketCount = PacketCount;
    rs->PacketSize = PacketSize;
    rs->FirstPacketOffset = allocSize - PacketSize;
    *Packets = packets;
    return STATUS_SUCCESS;
}

static VOID AudioEvtFreeRtPackets(_In_ ACXSTREAM Stream, _In_ PACX_RTPACKET Packets, _In_ ULONG PacketCount)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);
    ULONG i;

    PAGED_CODE();
    for (i = 0; i < PacketCount; i++) {
        PMDL mdl = Packets[i].RtPacketBuffer.u.MdlType.Mdl;
        if (mdl) {
            PVOID buffer = MmGetMdlVirtualAddress(mdl);
            IoFreeMdl(mdl);
            ExFreePoolWithTag(buffer, LDACA2DP_POOL_TAG);
        }
        if (i < MAX_RT_PACKETS)
            rs->Packets[i] = NULL;
    }
    rs->PacketCount = 0;
    ExFreePoolWithTag(Packets, LDACA2DP_POOL_TAG);
}

static ULONGLONG NowHns(_In_ PRENDER_STREAM Rs)
{
    return KSCONVERT_PERFORMANCE_TIME(Rs->QpcFrequency.QuadPart, KeQueryPerformanceCounter(NULL));
}

static VOID UpdatePosition(_In_ PRENDER_STREAM Rs)
{
    ULONGLONG elapsed;

    if (Rs->State != AcxStreamStateRun)
        return;
    elapsed = NowHns(Rs) - Rs->StartTime;
    elapsed = elapsed > Rs->GlitchAdjust ? elapsed - Rs->GlitchAdjust : 0;
    Rs->Position = Rs->StartPosition + elapsed * Rs->BytesPerSec / HNS_PER_SEC;
}

static VOID ScheduleNextPacket(_In_ PRENDER_STREAM Rs);

/* One packet's worth of time has passed: the packet the engine wrote is "played". */
static VOID PacketDone(_In_ PRENDER_STREAM Rs)
{
    ULONG current = (ULONG)InterlockedCompareExchange(&Rs->CurrentPacket, 0, 0);
    ULONG index = current % Rs->PacketCount;
    PUCHAR data = (PUCHAR)Rs->Packets[index] + (index == 0 ? Rs->FirstPacketOffset : 0);

    PcmWrite(Rs->Dev, data, Rs->PacketSize);
    InterlockedIncrement(&Rs->CurrentPacket);
    (VOID)AcxRtStreamNotifyPacketComplete(Rs->Stream, current, (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart);
    ScheduleNextPacket(Rs);
}

static VOID ScheduleNextPacket(_In_ PRENDER_STREAM Rs)
{
    ULONGLONG next = (ULONGLONG)(ULONG)InterlockedCompareExchange(&Rs->CurrentPacket, 0, 0) + 1;
    ULONGLONG due = Rs->StartTime + Rs->GlitchAdjust +
                    (next * Rs->PacketSize - Rs->StartPosition) * HNS_PER_SEC / Rs->BytesPerSec;
    ULONGLONG now = NowHns(Rs);

    if (due <= now) { /* late (timer jitter): catch up now, remember the slip */
        Rs->GlitchAdjust += now - due;
        PacketDone(Rs);
        return;
    }
    WdfTimerStart(Rs->Timer, -(LONGLONG)(due - now));
}

static VOID AudioEvtPacketTimer(_In_ WDFTIMER Timer)
{
    PacketDone(GetStreamTimerContext(Timer)->Stream);
}

static NTSTATUS AudioEvtStreamPrepareHardware(_In_ ACXSTREAM Stream)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);
    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    PAGED_CODE();
    if (rs->State != AcxStreamStateStop)
        return rs->State == AcxStreamStatePause ? STATUS_SUCCESS : STATUS_INVALID_STATE_TRANSITION;

    WDF_TIMER_CONFIG_INIT(&timerConfig, AudioEvtPacketTimer);
    timerConfig.AutomaticSerialization = TRUE;
    timerConfig.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, STREAM_TIMER_CONTEXT);
    attributes.ParentObject = Stream;
    status = WdfTimerCreate(&timerConfig, &attributes, &rs->Timer);
    if (!NT_SUCCESS(status))
        return status;
    GetStreamTimerContext(rs->Timer)->Stream = rs;
    PcmSetFormat(rs->Dev, rs->Format, FALSE);
    rs->State = AcxStreamStatePause;
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtStreamReleaseHardware(_In_ ACXSTREAM Stream)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);

    PAGED_CODE();
    if (rs->State == AcxStreamStateStop)
        return STATUS_SUCCESS;
    if (rs->Timer) {
        WdfTimerStop(rs->Timer, TRUE);
        WdfObjectDelete(rs->Timer);
        rs->Timer = NULL;
    }
    KeFlushQueuedDpcs();
    rs->Position = 0;
    rs->GlitchAdjust = 0;
    rs->CurrentPacket = 0;
    rs->State = AcxStreamStateStop;
    PcmSetRunning(rs->Dev, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtStreamRun(_In_ ACXSTREAM Stream)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);

    PAGED_CODE();
    if (rs->State == AcxStreamStateRun)
        return STATUS_SUCCESS;
    if (rs->State != AcxStreamStatePause)
        return STATUS_INVALID_STATE_TRANSITION;
    rs->StartTime = NowHns(rs);
    rs->StartPosition = rs->Position;
    rs->GlitchAdjust = 0;
    rs->State = AcxStreamStateRun;
    PcmSetFormat(rs->Dev, rs->Format, TRUE);
    ScheduleNextPacket(rs);
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtStreamPause(_In_ ACXSTREAM Stream)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);

    PAGED_CODE();
    if (rs->State == AcxStreamStatePause)
        return STATUS_SUCCESS;
    if (rs->State != AcxStreamStateRun)
        return STATUS_INVALID_STATE_TRANSITION;
    WdfTimerStop(rs->Timer, TRUE);
    UpdatePosition(rs);
    rs->State = AcxStreamStatePause;
    PcmSetRunning(rs->Dev, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtGetHwLatency(_In_ ACXSTREAM Stream, _Out_ ULONG *FifoSize, _Out_ ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    PAGED_CODE();
    *FifoSize = 0;
    *Delay = 0; /* the Bluetooth part is reported by the sink's AVDTP delay */
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtSetRenderPacket(_In_ ACXSTREAM Stream, _In_ ULONG Packet, _In_ ULONG Flags,
                                        _In_ ULONG EosPacketLength)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);
    ULONG current = (ULONG)InterlockedCompareExchange(&rs->CurrentPacket, 0, 0);

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);
    PAGED_CODE();
    if (Packet <= current)
        return STATUS_DATA_LATE_ERROR;
    if (Packet > current + 1)
        return STATUS_DATA_OVERRUN;
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtGetCurrentPacket(_In_ ACXSTREAM Stream, _Out_ PULONG CurrentPacket)
{
    PAGED_CODE();
    *CurrentPacket = (ULONG)InterlockedCompareExchange(&GetRenderStream(Stream)->CurrentPacket, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS AudioEvtGetPresentationPosition(_In_ ACXSTREAM Stream, _Out_ PULONGLONG PositionInBlocks,
                                                _Out_ PULONGLONG QPCPosition)
{
    PRENDER_STREAM rs = GetRenderStream(Stream);

    PAGED_CODE();
    UpdatePosition(rs);
    *PositionInBlocks = rs->Position / rs->BlockAlign;
    *QPCPosition = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}
