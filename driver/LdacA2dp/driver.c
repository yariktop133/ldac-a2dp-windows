#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, LdacA2dpEvtDeviceAdd)
#pragma alloc_text(PAGE, LdacA2dpEvtPrepareHardware)
#pragma alloc_text(PAGE, LdacA2dpEvtReleaseHardware)
#endif

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    ACX_DRIVER_CONFIG acxConfig;
    WDFDRIVER driver;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, LdacA2dpEvtDeviceAdd);
    config.DriverPoolTag = LDACA2DP_POOL_TAG;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status))
        return status;
    ACX_DRIVER_CONFIG_INIT(&acxConfig);
    return AcxDriverInitialize(driver, &acxConfig);
}

NTSTATUS LdacA2dpEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_TIMER_CONFIG timerConfig;
    ACX_DEVICEINIT_CONFIG acxInitConfig;
    ACX_DEVICE_CONFIG acxConfig;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = LdacA2dpEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = LdacA2dpEvtReleaseHardware;
    pnp.EvtDeviceSelfManagedIoInit = LdacA2dpEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = LdacA2dpEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attributes);

    /* The device is also the headphones' audio endpoint (audio.c). */
    ACX_DEVICEINIT_CONFIG_INIT(&acxInitConfig);
    status = AcxDeviceInitInitialize(DeviceInit, &acxInitConfig);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
        return status;
    ACX_DEVICE_CONFIG_INIT(&acxConfig);
    status = AcxDeviceInitialize(device, &acxConfig);
    if (!NT_SUCCESS(status))
        return status;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &pnpCaps);

    ctx = GetDeviceContext(device);
    ctx->Device = device;
    ctx->IoTarget = WdfDeviceGetIoTarget(device);
    ctx->Sig.Device = ctx;
    ctx->Sig.Kind = ChannelSignaling;
    ctx->Media.Device = ctx;
    ctx->Media.Kind = ChannelMedia;
    a2dp_prefs_default(&ctx->Prefs);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->Lock);
    if (!NT_SUCCESS(status))
        return status;

    WDF_TIMER_CONFIG_INIT(&timerConfig, LdacA2dpEvtSignalTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &attributes, &ctx->SignalTimer);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = LdacA2dpEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;

    status = AudioCreate(ctx);
    if (!NT_SUCCESS(status))
        return status;

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_LDACA2DP, NULL);
}

NTSTATUS LdacA2dpEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                     (PINTERFACE)&ctx->ProfileInterface, sizeof(ctx->ProfileInterface),
                                     BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status))
        return status;

    status = L2capInitialize(ctx);
    if (!NT_SUCCESS(status))
        return status;
    AudioNameFromBluetooth(ctx); /* before the endpoint gets built */
    return AcxDeviceAddCircuit(Device, ctx->Render);
}

NTSTATUS LdacA2dpEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();
    if (ctx->Render)
        (VOID)AcxDeviceRemoveCircuit(Device, ctx->Render);
    return STATUS_SUCCESS;
}

NTSTATUS LdacA2dpEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS status = L2capQueryAddresses(ctx);

    if (!NT_SUCCESS(status))
        return status;

    WdfSpinLockAcquire(ctx->Lock);
    avdtp_src_init(&ctx->Avdtp, &LdacA2dpAvdtpOps, ctx, &ctx->Prefs);
    ctx->Avdtp.auto_start = 0; /* START follows the audio, driven by the service */
    WdfSpinLockRelease(ctx->Lock);

    /* Accept AVDTP channels the sink opens itself (typical after it reconnects). */
    status = L2capRegisterServer(ctx);
    if (!NT_SUCCESS(status))
        return status;

    /* Like the captured reference session, configure the stream right away with
     * default preferences; the service may reconnect with its own via
     * IOCTL_LDACA2DP_CONNECT. The open is asynchronous, so a sink that is off or
     * out of range does not hold up device start; the failure lands in LastError. */
    status = L2capOpenChannel(&ctx->Sig);
    if (!NT_SUCCESS(status)) {
        WdfSpinLockAcquire(ctx->Lock);
        ctx->LastError = status;
        WdfSpinLockRelease(ctx->Lock);
    }
    return STATUS_SUCCESS;
}

VOID LdacA2dpEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    WdfTimerStop(ctx->SignalTimer, TRUE);
    L2capUnregisterServer(ctx); /* no new incoming channels from here on */
    L2capCloseChannelSync(&ctx->Media);
    L2capCloseChannelSync(&ctx->Sig); /* completes the pending signaling read */
}

VOID LdacA2dpEvtSignalTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfTimerGetParentObject(Timer));

    WdfSpinLockAcquire(ctx->Lock);
    if (ctx->Avdtp.state == AVDTP_SRC_WAIT_MEDIA && !ctx->Avdtp.pend_active) {
        ctx->LastError = STATUS_IO_TIMEOUT; /* media channel never came up */
        (VOID)avdtp_src_abort(&ctx->Avdtp);
    } else {
        avdtp_src_timeout(&ctx->Avdtp);
    }
    WdfSpinLockRelease(ctx->Lock);
    AvdtpRunDeferred(ctx);
}
